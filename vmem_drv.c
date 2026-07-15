// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_drv.c - vMem Linux kernel driver (v2.1)
 *
 * Consumer (peer) side only. The KMD imports the Astera vFE driver and calls
 * its in-kernel lookup to synthesize physical addresses:
 *
 *   vmem_get_dmabuf_fd input: {node_id, gpu_id, offset list}
 *   KMD flow: astera_lookup(node_id, gpu_id)
 *             -> (dbdf, bar, index)
 *             -> base = pci_resource_start(dbdf, bar) + index * 32 GB
 *             -> PA' = base + offset
 *             -> build MMIO dma-buf
 *
 * Device: /dev/vmemIntel
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fdtable.h>

#include "include/vmem_ioctl.h"
#include "vmem_dmabuf.h"
#include "vmem_astera.h"

#define VMEM_DRV_NAME    "vmemIntel"
#define VMEM_DRV_VERSION "2.1.0"

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
 * GET_IPC_HANDLE: receive {node_id, gpu_id, BAR-relative offsets} from peer UMD,
 * call vmem_get_ipc_handle() which internally calls astera_lookup() to compute
 * absolute PAs, then returns a pseudo dma-buf fd.
 */
static long vmem_ioctl_get_ipc_handle(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_get_ipc_handle_arg karg;
	struct dma_buf *dmabuf;
	int fd;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	dmabuf = vmem_get_ipc_handle(
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

/* PUT_IPC_HANDLE: close the pseudo dma-buf fd */
static long vmem_ioctl_put_ipc_handle(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_put_ipc_handle_arg karg;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	return close_fd(karg.fd);
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
	case VMEM_IOCTL_GET_IPC_HANDLE:
		return vmem_ioctl_get_ipc_handle(filp, arg);
	case VMEM_IOCTL_PUT_IPC_HANDLE:
		return vmem_ioctl_put_ipc_handle(filp, arg);
	case VMEM_IOCTL_IDENTIFY_DMABUF:
		return vmem_ioctl_identify_dmabuf(filp, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vmem_fops = {
	.owner          = THIS_MODULE,
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
MODULE_DESCRIPTION("vMem: PCIe cross-node GPU dma-buf via Astera vFE in-kernel lookup");
MODULE_VERSION(VMEM_DRV_VERSION);
MODULE_IMPORT_NS("DMA_BUF");
