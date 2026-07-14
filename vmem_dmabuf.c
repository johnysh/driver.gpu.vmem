// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_dmabuf.c - vMem dma-buf parse (producer) and assemble (consumer)
 *
 * v1.0 changes:
 *   - vmem_parse_dmabuf: userspace ptr output, dynamic count,
 *     persistent pin via vmem_import_pin; always returns BAR-relative offsets
 *   - vmem_create_dmabuf: takes PA list from daemon (no BDF/BAR in kernel)
 *   - vmem_identify_dmabuf_source: new — temporary attach + BAR scan
 *   - vmem_buf.entries: dynamically allocated (no fixed VMEM_MAX_PFN_ENTRIES limit)
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/err.h>

#include "vmem_dmabuf.h"

/* ==========================================================================
 * Common importer ops (used for both parse and identify paths)
 * ========================================================================== */

static void vmem_move_notify(struct dma_buf_attachment *attach)
{
	pr_warn("vmem: move_notify — buffer moved by exporter\n");
}

static const struct dma_buf_attach_ops vmem_importer_ops = {
	.allow_peer2peer = true,
	.move_notify     = vmem_move_notify,
};

/* ==========================================================================
 * Producer: vmem_parse_dmabuf
 * ========================================================================== */

/**
 * vmem_parse_dmabuf - attach to GPU dma-buf and extract scatter offsets
 *
 * On success the attachment is kept alive (persistent pin) so XE TTM cannot
 * evict the VRAM BO while the caller holds the PFN list.
 * Caller must call vmem_release_pin() (via PUT_IPC_FD) or close the vmem fd
 * to release the pin.
 *
 * @priv:        per-fd state (for pin storage)
 * @dmabuf_fd:   GPU dma-buf fd (from zeMemGetIpcHandle)
 * @bus/fn:      GPU PCI BDF
 * @entries_ptr: userspace output buffer for vmem_pfn_entry[]
 *               entries[i].offset = BAR2-relative byte offset from GPU VRAM base
 * @count:       IN = buffer capacity; OUT = actual entry count
 *
 * Returns 0, -ENOSPC (with *count = required), or negative error.
 */
int vmem_parse_dmabuf(struct vmem_file_priv *priv,
		      int dmabuf_fd,
		      u8 bus, u8 pci_devno, u8 fn,
		      struct vmem_pfn_entry __user *entries_ptr,
		      u32 *count)
{
	struct pci_dev *pdev = NULL;
	struct dma_buf *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;
	phys_addr_t bar2_base = 0;
	struct vmem_pfn_entry *kentries = NULL;
	u32 capacity = *count;
	u32 nr_entries;
	struct scatterlist *sg;
	int i, ret = 0;

	pdev = pci_get_domain_bus_and_slot(0, bus, PCI_DEVFN(pci_devno, fn));
	if (!pdev) {
		pr_err("vmem: GPU %02x:%02x.%x not found\n", bus, pci_devno, fn);
		return -ENODEV;
	}

	bar2_base = pci_resource_start(pdev, 2);
	if (!bar2_base) {
		pr_err("vmem: GPU BAR2 not available for %02x:%02x.%x\n",
		       bus, pci_devno, fn);
		ret = -ENXIO;
		goto put_pdev;
	}
	pr_debug("vmem: GPU BAR2 base = 0x%llx\n", (u64)bar2_base);

	dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto put_pdev;
	}

	/*
	 * XE driver requires DYNAMIC importers for LMEM dma-bufs.
	 * dma_buf_dynamic_attach sets attach->peer2peer = true, which
	 * allows the exporter to keep the BO in VRAM without migration.
	 */
	attach = dma_buf_dynamic_attach(dmabuf, &pdev->dev,
					&vmem_importer_ops, NULL);
	if (IS_ERR(attach)) {
		pr_err("vmem: dma_buf_dynamic_attach failed: %ld\n",
		       PTR_ERR(attach));
		ret = PTR_ERR(attach);
		goto put_dmabuf;
	}

	/*
	 * TODO: hard-pin via dma_buf_pin() when xe >= 6.19 is the minimum:
	 *   xe_dma_buf_pin() → xe_bo_pin_external() → ttm_bo_pin()
	 *   blocks TTM eviction without relying on persistent mapping.
	 *
	 * #if 0
	 * ret = dma_buf_pin(attach);
	 * if (ret) {
	 *     pr_err("vmem: dma_buf_pin failed: %d\n", ret);
	 *     goto detach;
	 * }
	 * #endif
	 *
	 * Until then, keeping the attach + sgt alive (soft-pin) prevents eviction.
	 */

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		pr_err("vmem: dma_buf_map_attachment failed: %ld\n",
		       PTR_ERR(sgt));
		ret = PTR_ERR(sgt);
		goto detach;
	}

	/* Report actual count and check capacity */
	nr_entries = sgt->nents;
	*count = nr_entries;

	if (capacity < nr_entries) {
		ret = -ENOSPC;
		goto unmap;
	}

	kentries = kvcalloc(nr_entries, sizeof(*kentries), GFP_KERNEL);
	if (!kentries) {
		ret = -ENOMEM;
		goto unmap;
	}

	/* Extract scatter offsets */
	i = 0;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		dma_addr_t dma_addr = sg_dma_address(sg);
		u32 size = (u32)sg_dma_len(sg);

		if (dma_addr < bar2_base) {
			pr_err("vmem: sg[%d] dma_addr 0x%llx < BAR2 0x%llx\n",
			       i, (u64)dma_addr, (u64)bar2_base);
			ret = -EINVAL;
			goto free_kentries;
		}

		kentries[i].offset = (u64)(dma_addr - bar2_base); /* BAR-relative */
		kentries[i].size   = size;
		kentries[i].pad    = 0;
		pr_debug("vmem: sg[%d]: offset=0x%llx size=%u\n",
			 i, kentries[i].offset, size);
	}

	pr_info("vmem: parsed %u BAR-relative entries from fd=%d (bar2=0x%llx)\n",
		nr_entries, dmabuf_fd, (u64)bar2_base);

	/* Copy to userspace before storing the pin */
	if (copy_to_user(entries_ptr, kentries,
			 nr_entries * sizeof(*kentries))) {
		ret = -EFAULT;
		goto free_kentries;
	}

	kvfree(kentries);
	kentries = NULL;

	/*
	 * Store persistent pin: keeps attach + sgt alive so XE TTM cannot
	 * evict the BO while the caller holds the PFN list (soft-pin).
	 */
	{
		struct vmem_import_pin *pin = kzalloc(sizeof(*pin), GFP_KERNEL);

		if (pin) {
			pin->dmabuf  = dmabuf;
			pin->attach  = attach;
			pin->sgt     = sgt;
			pin->pdev    = pdev;
			pin->orig_fd = dmabuf_fd;
			mutex_lock(&priv->pin_lock);
			list_add_tail(&pin->link, &priv->import_pins);
			mutex_unlock(&priv->pin_lock);
			pr_info("vmem: persistent pin stored for fd=%d\n",
				dmabuf_fd);
			return 0; /* resources owned by pin; call PUT_IPC_FD to release */
		}
		pr_warn("vmem: kzalloc(pin) failed; releasing mapping immediately\n");
		/* fall through: user has the data, no eviction protection */
		goto unmap;
	}

free_kentries:
	kvfree(kentries);
unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(dmabuf, attach);
put_dmabuf:
	dma_buf_put(dmabuf);
put_pdev:
	pci_dev_put(pdev);
	return ret;
}

/* ==========================================================================
 * Consumer: fake dma-buf implementation
 * ========================================================================== */

static struct sg_table *vmem_dmabuf_map(struct dma_buf_attachment *attach,
					enum dma_data_direction dir)
{
	struct vmem_buf *vbuf = attach->dmabuf->priv;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned int i;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, vbuf->nr_entries, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	sg = sgt->sgl;
	for (i = 0; i < vbuf->nr_entries; i++, sg = sg_next(sg)) {
		/* entries[i].offset = absolute PA (daemon: vFE_bar_base + scatter_offset) */
		phys_addr_t phys = (phys_addr_t)vbuf->entries[i].offset;
		size_t sz  = vbuf->entries[i].size;
		dma_addr_t dma;

		dma = dma_map_resource(attach->dev, phys, sz, dir,
				       DMA_ATTR_SKIP_CPU_SYNC);
		if (dma_mapping_error(attach->dev, dma)) {
			pr_err("vmem: dma_map_resource failed entry %u phys=0x%llx\n",
			       i, (u64)phys);
			/* unwind */
			{
				struct scatterlist *us = sgt->sgl;
				unsigned int j;

				for (j = 0; j < i; j++, us = sg_next(us))
					dma_unmap_resource(attach->dev,
							   sg_dma_address(us),
							   sg_dma_len(us), dir,
							   DMA_ATTR_SKIP_CPU_SYNC);
			}
			sg_free_table(sgt);
			kfree(sgt);
			return ERR_PTR(-EIO);
		}

		sg->page_link     = 0;
		sg->offset        = 0;
		sg->length        = sz;
		sg_dma_address(sg) = dma;
		sg_dma_len(sg)     = sz;
	}

	return sgt;
}

static void vmem_dmabuf_unmap(struct dma_buf_attachment *attach,
			      struct sg_table *sgt,
			      enum dma_data_direction dir)
{
	struct vmem_buf *vbuf = attach->dmabuf->priv;
	struct scatterlist *sg;
	unsigned int i;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (i >= vbuf->nr_entries)
			break;
		dma_unmap_resource(attach->dev,
				   sg_dma_address(sg),
				   vbuf->entries[i].size,
				   dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(sgt);
	kfree(sgt);
}

static void vmem_dmabuf_release(struct dma_buf *dmabuf)
{
	struct vmem_buf *vbuf = dmabuf->priv;

	pr_debug("vmem: releasing vmem_buf (nr_entries=%u)\n", vbuf->nr_entries);
	kvfree(vbuf->entries);
	kfree(vbuf);
}

static int vmem_dmabuf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attach)
{
	return 0;
}

static void vmem_dmabuf_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attach)
{
}

static int vmem_dmabuf_mmap(struct dma_buf *dmabuf,
			    struct vm_area_struct *vma)
{
	struct vmem_buf *vbuf = dmabuf->priv;
	unsigned long vaddr = vma->vm_start;
	unsigned int i;
	int ret;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	for (i = 0; i < vbuf->nr_entries; i++) {
		phys_addr_t phys = (phys_addr_t)vbuf->entries[i].offset; /* absolute PA */
		size_t sz = vbuf->entries[i].size;

		ret = io_remap_pfn_range(vma, vaddr, phys >> PAGE_SHIFT,
					 sz, vma->vm_page_prot);
		if (ret) {
			pr_err("vmem: io_remap_pfn_range failed entry %u\n", i);
			return ret;
		}
		vaddr += sz;
	}
	return 0;
}

static int vmem_dmabuf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction dir)
{
	return 0; /* BAR memory; no CPU cache flush needed */
}

static int vmem_dmabuf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction dir)
{
	return 0;
}

static const struct dma_buf_ops vmem_dmabuf_ops = {
	.attach            = vmem_dmabuf_attach,
	.detach            = vmem_dmabuf_detach,
	.map_dma_buf       = vmem_dmabuf_map,
	.unmap_dma_buf     = vmem_dmabuf_unmap,
	.release           = vmem_dmabuf_release,
	.mmap              = vmem_dmabuf_mmap,
	.begin_cpu_access  = vmem_dmabuf_begin_cpu_access,
	.end_cpu_access    = vmem_dmabuf_end_cpu_access,
};

/**
 * vmem_create_dmabuf - build a fake dma-buf from a physical address list
 *
 * @entries_ptr: entries[i].offset must be absolute physical addresses.
 *               The daemon computes PA = vFE_bar_base + scatter_offset using
 *               the Astera vFE driver before calling this ioctl.
 *               The vmem driver maps each PA directly — no BDF or BAR lookup.
 */
struct dma_buf *vmem_create_dmabuf(struct vmem_pfn_entry __user *entries_ptr,
				   u32 count)
{
	struct vmem_buf *vbuf;
	struct vmem_pfn_entry *kentries;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	size_t total = 0;
	unsigned int i;

	if (!count) {
		pr_err("vmem: GET_IPC_FD: count=0\n");
		return ERR_PTR(-EINVAL);
	}

	kentries = kvcalloc(count, sizeof(*kentries), GFP_KERNEL);
	if (!kentries)
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(kentries, entries_ptr, count * sizeof(*kentries))) {
		kvfree(kentries);
		return ERR_PTR(-EFAULT);
	}

	for (i = 0; i < count; i++)
		total += kentries[i].size;

	vbuf = kzalloc(sizeof(*vbuf), GFP_KERNEL);
	if (!vbuf) {
		kvfree(kentries);
		return ERR_PTR(-ENOMEM);
	}

	vbuf->nr_entries   = count;
	vbuf->total_size   = total;
	vbuf->entries      = kentries; /* entries[i].offset = absolute PA */ /* ownership transferred to vbuf */

	exp_info.ops   = &vmem_dmabuf_ops;
	exp_info.size  = total;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv  = vbuf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		pr_err("vmem: dma_buf_export failed: %ld\n", PTR_ERR(dmabuf));
		kvfree(kentries);
		kfree(vbuf);
		return dmabuf;
	}

	pr_info("vmem: fake dma_buf created: %u entries %zu bytes (PA from daemon)\n",
		count, total);
	return dmabuf;
}

/* ==========================================================================
 * Source detection: vmem_identify_dmabuf_source
 * ========================================================================== */

/**
 * vmem_identify_dmabuf_source - probe which GPU's VRAM backs a dma-buf
 *
 * Does a temporary attach + map, reads the first DMA address, scans all
 * PCI BARs (BAR2 first, then BAR0) to find the owner, then cleans up.
 */
int vmem_identify_dmabuf_source(int fd,
				u8 bus, u8 dev, u8 fn,
				u8 *src_bus, u8 *src_dev, u8 *src_fn)
{
	struct pci_dev *pdev, *iter = NULL;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct scatterlist *sg;
	dma_addr_t first_dma;
	phys_addr_t first_phys;
	int ret;

	pdev = pci_get_domain_bus_and_slot(0, bus, PCI_DEVFN(dev, fn));
	if (!pdev)
		return -ENODEV;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		pci_dev_put(pdev);
		return PTR_ERR(dmabuf);
	}

	attach = dma_buf_dynamic_attach(dmabuf, &pdev->dev,
					&vmem_importer_ops, NULL);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto put_buf;
	}

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto detach;
	}

	sg = sgt->sgl;
	if (!sg || !sgt->nents) {
		ret = -EINVAL;
		goto unmap;
	}
	first_dma  = sg_dma_address(sg);
	first_phys = (phys_addr_t)first_dma;

	/* Scan PCI BARs: BAR2 (GPU LMEM on Intel Arc) then BAR0 */
	ret = -ENODEV;
	*src_bus = *src_dev = *src_fn = 0;
	while ((iter = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, iter))) {
		resource_size_t b2s = pci_resource_start(iter, 2);
		resource_size_t b2e = pci_resource_end(iter, 2);
		resource_size_t b0s = pci_resource_start(iter, 0);
		resource_size_t b0e = pci_resource_end(iter, 0);

		if ((b2s && first_phys >= b2s && first_phys <= b2e) ||
		    (b0s && first_phys >= b0s && first_phys <= b0e)) {
			*src_bus  = iter->bus->number;
			*src_dev  = PCI_SLOT(iter->devfn);
			*src_fn   = PCI_FUNC(iter->devfn);
			pci_dev_put(iter);
			ret = 0;
			pr_info("vmem: dma-buf fd=%d owned by %02x:%02x.%x\n",
				fd, *src_bus, *src_dev, *src_fn);
			break;
		}
	}

unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(dmabuf, attach);
put_buf:
	dma_buf_put(dmabuf);
	pci_dev_put(pdev);
	return ret;
}

/* ==========================================================================
 * Persistent mapping lifecycle
 * ========================================================================== */

void vmem_import_pins_cleanup(struct vmem_file_priv *priv)
{
	struct vmem_import_pin *pin, *tmp;

	mutex_lock(&priv->pin_lock);
	list_for_each_entry_safe(pin, tmp, &priv->import_pins, link) {
		list_del(&pin->link);
		dma_buf_unmap_attachment_unlocked(pin->attach, pin->sgt,
						  DMA_BIDIRECTIONAL);
		dma_buf_detach(pin->dmabuf, pin->attach);
		dma_buf_put(pin->dmabuf);
		pci_dev_put(pin->pdev);
		pr_debug("vmem: cleanup released pin orig_fd=%d\n", pin->orig_fd);
		kfree(pin);
	}
	mutex_unlock(&priv->pin_lock);
}

int vmem_release_pin(struct vmem_file_priv *priv, int orig_fd)
{
	struct vmem_import_pin *pin, *tmp;
	struct vmem_import_pin *found = NULL;

	mutex_lock(&priv->pin_lock);
	list_for_each_entry_safe(pin, tmp, &priv->import_pins, link) {
		if (pin->orig_fd == orig_fd) {
			list_del(&pin->link);
			found = pin;
			break;
		}
	}
	mutex_unlock(&priv->pin_lock);

	if (!found)
		return -ENOENT;

	dma_buf_unmap_attachment_unlocked(found->attach, found->sgt,
					  DMA_BIDIRECTIONAL);
	dma_buf_detach(found->dmabuf, found->attach);
	dma_buf_put(found->dmabuf);
	pci_dev_put(found->pdev);
	pr_debug("vmem: released pin orig_fd=%d\n", orig_fd);
	kfree(found);
	return 0;
}
