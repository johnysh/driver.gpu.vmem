// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_drv.c - vMem Linux kernel driver (v3.1)
 *
 * IOCTL surface (Method 1 - KMD PA synthesis via Astera vFE):
 *   VMEM_IOCTL_VERSION       -- query driver version
 *   VMEM_IOCTL_OPEN_DMABUF   -- origin: P2P attach, return absolute PAs (abs sg_dma_address)
 *                               UMD computes: offset[i] = pa[i] - gpu_bar2_base
 *   VMEM_IOCTL_CLOSE_DMABUF  -- origin: release soft-pin
 *   VMEM_IOCTL_GET_DMABUF    -- peer: BAR-relative offsets -> astera_lookup -> PA synthesis
 *   VMEM_IOCTL_PUT_DMABUF    -- peer: destroy pseudo dma-buf
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

#include "include/vmem_ioctl.h"
#include "vmem_dmabuf.h"
#include "vmem_astera.h"
#include "vmem_debugfs.h"

#define VMEM_DRV_NAME    "vmemIntel"
#define VMEM_DRV_VERSION "3.1.0"

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

/*
 * Origin: OPEN_DMABUF
 * KMD attaches GPU dma-buf, walks sg_table, returns ABSOLUTE physical addresses.
 * UMD then computes BAR-relative offsets: offset[i] = pa[i] - bar2_base.
 */
static long vmem_ioctl_open_dmabuf(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_open_dmabuf_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	ret = vmem_open_dmabuf_fd(priv,
				  karg.fd, karg.bus, karg.device, karg.function,
				  (struct vmem_pfn_entry __user *)(uintptr_t)karg.entries_ptr,
				  &karg.count, &karg.page_size, &karg.total_size);

	/* Always write back count (and page_size/total_size on success) */
	if (copy_to_user((void __user *)arg, &karg, sizeof(karg)))
		return -EFAULT;
	return ret;
}

static long vmem_ioctl_close_dmabuf(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_close_dmabuf_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;
	ret = vmem_close_dmabuf(priv, karg.fd);
	if (ret)
		pr_warn("vmem: CLOSE_DMABUF fd=%d: no matching pin (%d)\n", karg.fd, ret);
	return ret;
}

/*
 * Peer: GET_DMABUF (Method 1)
 * Input: {node_id, gpu_id, BAR-relative offset entries}
 * KMD: astera_lookup -> vfe_base -> PA'[i] = vfe_base + entries[i].addr -> pseudo dma-buf fd
 */
static long vmem_ioctl_get_dmabuf(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_get_dmabuf_arg karg;
	int fd;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	fd = vmem_get_dmabuf_fd(priv,
				karg.node_id, karg.gpu_id,
				(struct vmem_pfn_entry __user *)(uintptr_t)karg.entries_ptr,
				karg.count);
	if (fd < 0) return fd;

	karg.fd = fd;
	if (copy_to_user((void __user *)arg, &karg, sizeof(karg))) {
		vmem_put_dmabuf(priv, fd);
		return -EFAULT;
	}
	return 0;
}

static long vmem_ioctl_put_dmabuf(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_put_dmabuf_arg karg;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;
	return vmem_put_dmabuf(priv, karg.fd);
}

static long vmem_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case VMEM_IOCTL_VERSION:      return vmem_ioctl_version(filp, arg);
	case VMEM_IOCTL_OPEN_DMABUF:  return vmem_ioctl_open_dmabuf(filp, arg);
	case VMEM_IOCTL_CLOSE_DMABUF: return vmem_ioctl_close_dmabuf(filp, arg);
	case VMEM_IOCTL_GET_DMABUF:   return vmem_ioctl_get_dmabuf(filp, arg);
	case VMEM_IOCTL_PUT_DMABUF:   return vmem_ioctl_put_dmabuf(filp, arg);
	default: return -ENOTTY;
	}
}

static int vmem_open(struct inode *inode, struct file *filp)
{
	struct vmem_file_priv *priv = kzalloc(sizeof(*priv), GFP_KERNEL);

	if (!priv) return -ENOMEM;
	INIT_LIST_HEAD(&priv->import_pins);
	mutex_init(&priv->pin_lock);
	INIT_LIST_HEAD(&priv->buffers);
	mutex_init(&priv->buf_lock);
	filp->private_data = priv;
	return 0;
}

static int vmem_release(struct inode *inode, struct file *filp)
{
	struct vmem_file_priv *priv = filp->private_data;

	if (priv) {
		vmem_import_pins_cleanup(priv);
		vmem_buffers_cleanup(priv);
		mutex_destroy(&priv->pin_lock);
		mutex_destroy(&priv->buf_lock);
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

static int __init vmem_init(void)
{
	int ret = misc_register(&vmem_misc);

	if (ret) { pr_err("vmem: misc_register failed: %d\n", ret); return ret; }
	dma_coerce_mask_and_coherent(vmem_misc.this_device, DMA_BIT_MASK(64));
	vmem_debugfs_init();
	pr_info("vmem: driver v%s loaded, /dev/%s (minor %d)\n",
		VMEM_DRV_VERSION, VMEM_DRV_NAME, vmem_misc.minor);
	pr_info("vmem: OPEN_DMABUF returns abs PAs; UMD computes BAR-relative offsets\n");
	pr_info("vmem: GET_DMABUF returns -ENOSYS until astera_vfe.ko registers\n");
	return 0;
}

static void __exit vmem_exit(void)
{
	vmem_debugfs_exit();
	misc_deregister(&vmem_misc);
	pr_info("vmem: driver unloaded\n");
}

module_init(vmem_init);
module_exit(vmem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("vMem: PCIe cross-node GPU dma-buf translator (Method 1 KMD PA synthesis)");
MODULE_VERSION(VMEM_DRV_VERSION);
MODULE_IMPORT_NS("DMA_BUF");
