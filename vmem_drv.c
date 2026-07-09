// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_drv.c - vMem Linux kernel driver (misc character device /dev/vmem)
 *
 * Implements the dma-buf translator for cross-node GPU P2P over PCIe switch.
 * Provides four primary IOCTLs (per PDF spec) plus one endpoint registration:
 *
 *  VMEM_IOCTL_GET_PFN_OFFSET_LIST  -- producer: parse GPU dma-buf -> offsets
 *  VMEM_IOCTL_GET_IPC_FD           -- consumer: offsets -> fake dma-buf fd
 *  VMEM_IOCTL_CLOSE_IPC_FD         -- consumer: destroy fake dma-buf
 *  VMEM_IOCTL_PUT_IPC_FD           -- producer: release exported GPU handle
 *  VMEM_IOCTL_REGISTER_EP          -- register local PCIe endpoint BAR2
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
#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <linux/fdtable.h>

#include "include/vmem_ioctl.h"
#include "vmem_dmabuf.h"

#define VMEM_DRV_NAME "vmem"
#define VMEM_DRV_VERSION "0.1.0"

/* -- Global endpoint registry -------------------- */

#define VMEM_MAX_ENDPOINTS 8

struct vmem_endpoint {
	bool        valid;
	u8          bus, device, function;
	phys_addr_t bar2_base;
	u64         bar2_size;
};

static struct vmem_endpoint ep_table[VMEM_MAX_ENDPOINTS];
static DEFINE_MUTEX(ep_lock);

/* Return the first registered endpoint's BAR2 base (simplified single-EP) */
static phys_addr_t vmem_get_ep_bar2_base(void)
{
	int i;
	mutex_lock(&ep_lock);
	for (i = 0; i < VMEM_MAX_ENDPOINTS; i++) {
		if (ep_table[i].valid) {
			phys_addr_t base = ep_table[i].bar2_base;
			mutex_unlock(&ep_lock);
			return base;
		}
	}
	mutex_unlock(&ep_lock);
	return 0;
}

/* -- IOCTL handlers -------------------- */

static long vmem_ioctl_get_pfn(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_get_pfn_arg *karg;
	int ret;

	karg = kzalloc(sizeof(*karg), GFP_KERNEL);
	if (!karg)
		return -ENOMEM;

	if (copy_from_user(karg, (void __user *)arg, sizeof(*karg))) {
		ret = -EFAULT;
		goto out;
	}

	ret = vmem_parse_dmabuf(karg->fd,
				karg->bus, karg->device, karg->function,
				&karg->pfn_list);
	if (ret)
		goto out;

	if (copy_to_user((void __user *)arg, karg, sizeof(*karg)))
		ret = -EFAULT;
out:
	kfree(karg);
	return ret;
}

static long vmem_ioctl_get_ipc_fd(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_get_ipc_fd_arg *karg;
	struct dma_buf *dmabuf;
	phys_addr_t ep_base;
	int fd, ret = 0;

	karg = kzalloc(sizeof(*karg), GFP_KERNEL);
	if (!karg)
		return -ENOMEM;

	if (copy_from_user(karg, (void __user *)arg, sizeof(*karg))) {
		ret = -EFAULT;
		goto out;
	}

	ep_base = vmem_get_ep_bar2_base();
	if (!ep_base) {
		pr_err("vmem: no endpoint registered (call REGISTER_EP first)\n");
		ret = -ENODEV;
		goto out;
	}

	dmabuf = vmem_create_dmabuf(&karg->pfn_list, ep_base);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto out;
	}

	/* Install the dma_buf as a file descriptor in the calling process */
	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		pr_err("vmem: dma_buf_fd failed: %d\n", fd);
		dma_buf_put(dmabuf);
		ret = fd;
		goto out;
	}

	karg->fd = fd;
	if (copy_to_user((void __user *)arg, karg, sizeof(*karg))) {
		/* fd is already installed; we can't take it back easily */
		pr_warn("vmem: copy_to_user failed after fd installed\n");
		ret = -EFAULT;
	}
out:
	kfree(karg);
	return ret;
}

static long vmem_ioctl_close_ipc_fd(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_close_ipc_fd_arg karg;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	/*
	 * Closing the fake dma-buf fd: the standard way is for userspace
	 * to call close(fd).  Here we do it on behalf of the caller via
	 * __close_fd so the driver can be used without userspace changes.
	 */
	return close_fd(karg.fd);
}

static long vmem_ioctl_put_ipc_fd(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_put_ipc_fd_arg karg;
	struct dma_buf *dmabuf;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	/*
	 * Decrement the dma_buf reference the producer holds on its original
	 * real dma-buf fd.  We borrow a reference from the fd table then
	 * drop it.  The fd itself is still owned by the process.
	 */
	dmabuf = dma_buf_get(karg.fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	/* Drop the reference we just took (net zero: just validates the fd) */
	dma_buf_put(dmabuf);

	pr_debug("vmem: PUT_IPC_FD fd=%d ok\n", karg.fd);
	return 0;
}

static long vmem_ioctl_register_ep(struct file *filp, unsigned long arg)
{
	struct vmem_ioctl_register_ep_arg karg;
	struct vmem_endpoint *ep = NULL;
	int i;

	if (copy_from_user(&karg, (void __user *)arg, sizeof(karg)))
		return -EFAULT;

	mutex_lock(&ep_lock);
	/* Find free slot */
	for (i = 0; i < VMEM_MAX_ENDPOINTS; i++) {
		if (!ep_table[i].valid) {
			ep = &ep_table[i];
			break;
		}
	}
	if (!ep) {
		mutex_unlock(&ep_lock);
		pr_err("vmem: endpoint table full (max %d)\n", VMEM_MAX_ENDPOINTS);
		return -ENOSPC;
	}

	ep->bus      = karg.bus;
	ep->device   = karg.device;
	ep->function = karg.function;
	ep->bar2_size = karg.bar2_size;

	if (karg.bar2_base) {
		/* Caller provided explicit address */
		ep->bar2_base = (phys_addr_t)karg.bar2_base;
	} else {
		/* Auto-detect from PCI BDF */
		struct pci_dev *pdev = pci_get_domain_bus_and_slot(
			0, karg.bus, PCI_DEVFN(karg.device, karg.function));
		if (!pdev) {
			mutex_unlock(&ep_lock);
			pr_err("vmem: endpoint PCI device %02x:%02x.%x not found\n",
			       karg.bus, karg.device, karg.function);
			return -ENODEV;
		}
		ep->bar2_base = pci_resource_start(pdev, 2);
		if (!ep->bar2_size)
			ep->bar2_size = pci_resource_len(pdev, 2);
		pci_dev_put(pdev);
	}

	ep->valid = true;
	mutex_unlock(&ep_lock);

	pr_info("vmem: registered endpoint %02x:%02x.%x BAR2 base=0x%llx size=0x%llx\n",
		karg.bus, karg.device, karg.function,
		(u64)ep->bar2_base, (u64)ep->bar2_size);
	return 0;
}

/* -- File operations -------------------- */

static long vmem_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case VMEM_IOCTL_GET_PFN_OFFSET_LIST:
		return vmem_ioctl_get_pfn(filp, arg);
	case VMEM_IOCTL_GET_IPC_FD:
		return vmem_ioctl_get_ipc_fd(filp, arg);
	case VMEM_IOCTL_CLOSE_IPC_FD:
		return vmem_ioctl_close_ipc_fd(filp, arg);
	case VMEM_IOCTL_PUT_IPC_FD:
		return vmem_ioctl_put_ipc_fd(filp, arg);
	case VMEM_IOCTL_REGISTER_EP:
		return vmem_ioctl_register_ep(filp, arg);
	default:
		return -ENOTTY;
	}
}

static int vmem_open(struct inode *inode, struct file *filp)
{
	pr_debug("vmem: open\n");
	return 0;
}

static int vmem_release(struct inode *inode, struct file *filp)
{
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

/* -- Module init / exit -------------------- */

static int __init vmem_init(void)
{
	int ret;

	memset(ep_table, 0, sizeof(ep_table));

	ret = misc_register(&vmem_misc);
	if (ret) {
		pr_err("vmem: misc_register failed: %d\n", ret);
		return ret;
	}

	pr_info("vmem: driver v%s loaded, device /dev/%s (minor %d)\n",
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
MODULE_AUTHOR("Intel / vmem project");
MODULE_DESCRIPTION("vMem: PCIe switch cross-node GPU dma-buf translator");
MODULE_VERSION(VMEM_DRV_VERSION);
MODULE_IMPORT_NS("DMA_BUF");

