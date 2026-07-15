// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_drv.c - vMem Linux kernel driver (v3.0)
 *
 * Device: /dev/vmemIntel
 *
 * IOCTL surface (Method 1 - address translation in KMD via Astera vFE):
 *   VMEM_IOCTL_VERSION       -- query driver version
 *   VMEM_IOCTL_OPEN_DMABUF   -- origin: attach GPU dma-buf, extract offsets, soft-pin
 *   VMEM_IOCTL_CLOSE_DMABUF  -- origin: release soft-pin (by opened fd)
 *   VMEM_IOCTL_GET_DMABUF    -- peer: astera_lookup -> PA synthesis -> pseudo dma-buf fd
 *   VMEM_IOCTL_PUT_DMABUF    -- peer: destroy pseudo dma-buf
 *
 * Backstop: vmem fd close (including process crash) sweeps all remaining
 *   import_pins via vmem_import_pins_cleanup() and all exported buffers via
 *   vmem_buffers_cleanup().
 *
 * Debugfs: /sys/kernel/debug/vmemIntel/{imported,exported}
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
#define VMEM_DRV_VERSION "3.0.0"

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

/*
 * Origin: attach GPU dma-buf, extract BAR-relative offsets, store soft-pin.
 * Two-step count negotiation: call with count=0 -> -ENOSPC + count=required;
 * allocate; call again to fill.
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
				  &karg.count);

	/* Always write back count (actual or required for -ENOSPC retry) */
	if (copy_to_user((void __user *)arg, &karg, sizeof(karg)))
		return -EFAULT;
	return ret;
}

/*
 * Origin: release persistent soft-pin kept since OPEN_DMABUF.
 * @fd: the GPU dma-buf fd originally passed to OPEN_DMABUF.
 */
static long vmem_ioctl_close_dmabuf(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_close_dmabuf_arg karg;
	int ret;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	ret = vmem_close_dmabuf(priv, karg.fd);
	if (ret)
		pr_warn("vmem: CLOSE_DMABUF fd=%d: no matching pin (%d)\n",
			karg.fd, ret);
	return ret;
}

/*
 * Peer: Method 1 - receive {node_id, gpu_id, BAR-relative offsets},
 * call vmem_get_dmabuf_fd() which internally runs astera_lookup() and
 * computes absolute PAs, then returns a pseudo dma-buf fd.
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
	if (fd < 0)
		return fd;

	karg.fd = fd;
	if (copy_to_user((void __user *)arg, &karg, sizeof(karg))) {
		pr_warn("vmem: copy_to_user failed after fd=%d installed\n", fd);
		return -EFAULT;
	}
	return 0;
}

/* Peer: destroy pseudo dma-buf fd returned by GET_DMABUF */
static long vmem_ioctl_put_dmabuf(struct file *filp, unsigned long arg)
{
	struct vmem_file_priv *priv = filp->private_data;
	struct vmem_ioctl_put_dmabuf_arg karg;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;
	return vmem_put_dmabuf(priv, karg.fd);
}

/* ── File operations ──────────────────────────────────────── */

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
		/* Backstop: sweep any un-released pins and exported buffers */
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

/* ── Module init / exit ───────────────────────────────────── */

static int __init vmem_init(void)
{
	int ret = misc_register(&vmem_misc);

	if (ret) { pr_err("vmem: misc_register failed: %d\n", ret); return ret; }
	dma_coerce_mask_and_coherent(vmem_misc.this_device, DMA_BIT_MASK(64));
	vmem_debugfs_init();
	pr_info("vmem: driver v%s loaded, /dev/%s (minor %d)\n",
		VMEM_DRV_VERSION, VMEM_DRV_NAME, vmem_misc.minor);
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
