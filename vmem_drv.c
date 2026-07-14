// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_drv.c - vMem Linux kernel driver (v1.0)
 *
 * Changes from v0.1:
 *   - REGISTER_EP / CLOSE_IPC_FD removed
 *   - GET_IPC_FD: consumer BDF per-call (no global ep_table)
 *   - GET_PFN_OFFSET_LIST: userspace pointer, dynamic count, ABS_PA flag
 *   - VERSION + IDENTIFY_DMABUF added
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fdtable.h>   /* close_fd() */

#include "include/vmem_ioctl.h"
#include "vmem_dmabuf.h"

#define VMEM_DRV_NAME    "vmem"
#define VMEM_DRV_VERSION "1.0.0"

/* ── IOCTL handlers ───────────────────────────────────────── */

static long vmem_ioctl_version(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_version_arg ver = {
		.major = VMEM_DRV_VERSION_MAJOR,
		.minor = VMEM_DRV_VERSION_MINOR,
		.patch = VMEM_DRV_VERSION_PATCH,
	};

	if (copy_to_user((void __user *)arg, &ver, sizeof(ver)))
		return -EFAULT;
	return 0;
}

static long vmem_ioctl_get_pfn(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_get_pfn_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	ret = vmem_parse_dmabuf(priv,
				karg.fd,
				karg.bus, karg.device, karg.function,
				karg.flags,
				(struct vmem_pfn_entry __user *)(uintptr_t)
					karg.entries_ptr,
				&karg.count);

	/* Always write back count (actual or required for ENOSPC retry) */
	if (copy_to_user((void __user *)arg, &karg, sizeof(karg)))
		return -EFAULT;

	return ret;
}

static long vmem_ioctl_get_ipc_fd(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_get_ipc_fd_arg karg;
	struct dma_buf *dmabuf;
	int fd;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	dmabuf = vmem_create_dmabuf(
		(struct vmem_pfn_entry __user *)(uintptr_t)karg.entries_ptr,
		karg.count,
		karg.consumer_bus, karg.consumer_device, karg.consumer_function,
		karg.flags);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		return fd;
	}

	karg.fd = fd;
	if (copy_to_user((void __user *)arg, &karg, sizeof(karg))) {
		pr_warn("vmem: copy_to_user failed after fd=%d installed\n", fd);
		return -EFAULT;
	}

	return 0;
}

static long vmem_ioctl_close_ipc_fd(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_close_ipc_fd_arg karg;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	/*
	 * Close the fake dma-buf fd on behalf of the caller.
	 * Standard userspace path is close(fd), but providing this ioctl
	 * lets the driver manage fd lifetime in coordination with the UMD.
	 */
	return close_fd(karg.fd);
}

static long vmem_ioctl_put_ipc_fd(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_put_ipc_fd_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	ret = vmem_release_pin(priv, karg.fd);
	if (ret)
		pr_warn("vmem: PUT_IPC_FD fd=%d: no matching pin (%d)\n",
			karg.fd, ret);
	else
		pr_debug("vmem: PUT_IPC_FD fd=%d: pin released\n", karg.fd);
	return ret;
}

static long vmem_ioctl_identify_dmabuf(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_identify_dmabuf_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	ret = vmem_identify_dmabuf_source(karg.fd,
					  karg.bus, karg.device, karg.function,
					  &karg.source_bus,
					  &karg.source_device,
					  &karg.source_function);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &karg, sizeof(karg)))
		return -EFAULT;

	return 0;
}

/* ── File operations ──────────────────────────────────────── */

static long vmem_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case VMEM_IOCTL_VERSION:
		return vmem_ioctl_version(filp, arg);
	case VMEM_IOCTL_GET_PFN_OFFSET_LIST:
		return vmem_ioctl_get_pfn(filp, arg);
	case VMEM_IOCTL_GET_IPC_FD:
		return vmem_ioctl_get_ipc_fd(filp, arg);
	case VMEM_IOCTL_CLOSE_IPC_FD:
		return vmem_ioctl_close_ipc_fd(filp, arg);
	case VMEM_IOCTL_PUT_IPC_FD:
		return vmem_ioctl_put_ipc_fd(filp, arg);
	case VMEM_IOCTL_IDENTIFY_DMABUF:
		return vmem_ioctl_identify_dmabuf(filp, arg);
	default:
		return -ENOTTY;
	}
}

static int vmem_open(struct inode *inode, struct file *filp)
{
	struct vmem_file_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	INIT_LIST_HEAD(&priv->import_pins);
	mutex_init(&priv->pin_lock);
	filp->private_data = priv;
	pr_debug("vmem: open\n");
	return 0;
}

static int vmem_release(struct inode *inode, struct file *filp)
{
	struct vmem_file_priv *priv = filp->private_data;

	if (priv) {
		vmem_import_pins_cleanup(priv);
		mutex_destroy(&priv->pin_lock);
		kfree(priv);
		filp->private_data = NULL;
	}
	pr_debug("vmem: close\n");
	return 0;
}

static const struct file_operations vmem_fops = {
	.owner          = THIS_MODULE,
	.open           = vmem_open,
	.release        = vmem_release,
	.unlocked_ioctl = vmem_ioctl,
	.compat_ioctl   = vmem_ioctl,
};

static struct miscdevice vmem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = VMEM_DRV_NAME,
	.fops  = &vmem_fops,
	.mode  = 0666,
};

/* ── Module init / exit ───────────────────────────────────── */

static int __init vmem_init(void)
{
	int ret;

	ret = misc_register(&vmem_misc);
	if (ret) {
		pr_err("vmem: misc_register failed: %d\n", ret);
		return ret;
	}

	dma_coerce_mask_and_coherent(vmem_misc.this_device, DMA_BIT_MASK(64));

	pr_info("vmem: driver v%s loaded, /dev/%s (minor %d)\n",
		VMEM_DRV_VERSION, VMEM_DRV_NAME, vmem_misc.minor);
	return 0;
}

static void __exit vmem_exit(void)
{
	misc_deregister(&vmem_misc);
	pr_info("vmem: driver unloaded\n");
}

module_init(vmem_init);
module_exit(vmem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("vMem: PCIe switch cross-node GPU dma-buf translator");
MODULE_VERSION(VMEM_DRV_VERSION);
MODULE_IMPORT_NS("DMA_BUF");
