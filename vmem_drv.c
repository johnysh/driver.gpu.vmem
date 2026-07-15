// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_drv.c - vMem Linux kernel driver (v2.0)
 *
 * Device: /dev/vmemIntel
 *
 * v2.0 changes:
 *   - OPEN_IPC_HANDLE  replaces GET_PFN_OFFSET_LIST (same logic, new name)
 *   - GET_IPC_HANDLE   replaces GET_IPC_FD: takes {node_id, gpu_id, offsets}
 *                      PA synthesis now done in KMD via vmem_astera_lookup()
 *   - PUT_IPC_HANDLE   replaces CLOSE_IPC_FD (peer teardown, same logic)
 *   - CLOSE_IPC_HANDLE replaces PUT_IPC_FD   (origin teardown, same logic)
 *   - VERSION + IDENTIFY_DMABUF unchanged
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
#include <linux/fdtable.h>

#include "include/vmem_ioctl.h"
#include "vmem_dmabuf.h"
#include "vmem_astera.h"

#define VMEM_DRV_NAME    "vmemIntel"
#define VMEM_DRV_VERSION "2.0.0"

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

/* Origin: attach GPU dma-buf, extract BAR-relative scatter offsets */
static long vmem_ioctl_open_ipc_handle(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_open_ipc_handle_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	ret = vmem_open_ipc_handle(priv,
				   karg.fd,
				   karg.bus, karg.device, karg.function,
				   (struct vmem_pfn_entry __user *)(uintptr_t)
					karg.entries_ptr,
				   &karg.count);

	/* Always write back count (actual or required for -ENOSPC retry) */
	if (copy_to_user((void __user *)arg, &karg, sizeof(karg)))
		return -EFAULT;

	return ret;
}

/*
 * Peer: Method 1 - receive {node_id, gpu_id, BAR-relative offsets},
 * call vmem_astera_lookup() in-kernel to compute PAs, build pseudo dma-buf.
 */
static long vmem_ioctl_get_ipc_handle(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_get_ipc_handle_arg karg;
	struct dma_buf *dmabuf;
	int fd;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	dmabuf = vmem_get_ipc_handle_method1(
		karg.node_id, karg.gpu_id,
		(struct vmem_pfn_entry __user *)(uintptr_t)karg.entries_ptr,
		karg.count);
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

/* Peer: destroy the pseudo dma-buf fd */
static long vmem_ioctl_put_ipc_handle(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_put_ipc_handle_arg karg;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	return close_fd(karg.fd);
}

/* Origin: release persistent soft-pin */
static long vmem_ioctl_close_ipc_handle(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_close_ipc_handle_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	ret = vmem_close_ipc_handle(priv, karg.fd);
	if (ret)
		pr_warn("vmem: CLOSE_IPC_HANDLE fd=%d: no matching pin (%d)\n",
			karg.fd, ret);
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
	case VMEM_IOCTL_OPEN_IPC_HANDLE:
		return vmem_ioctl_open_ipc_handle(filp, arg);
	case VMEM_IOCTL_GET_IPC_HANDLE:
		return vmem_ioctl_get_ipc_handle(filp, arg);
	case VMEM_IOCTL_PUT_IPC_HANDLE:
		return vmem_ioctl_put_ipc_handle(filp, arg);
	case VMEM_IOCTL_CLOSE_IPC_HANDLE:
		return vmem_ioctl_close_ipc_handle(filp, arg);
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
	pr_info("vmem: GET_IPC_HANDLE returns -ENOSYS until astera_vfe.ko registers\n");
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
MODULE_DESCRIPTION("vMem: PCIe switch cross-node GPU dma-buf translator (Method 1)");
MODULE_VERSION(VMEM_DRV_VERSION);
MODULE_IMPORT_NS("DMA_BUF");
