// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_dmabuf.c - vMem dma-buf parse (producer) and assemble (consumer)
 *
 * Producer flow (vmem_parse_dmabuf):
 *   fd -> dma_buf_get -> dma_buf_attach(GPU dev) -> dma_buf_map_attachment
 *   -> scatter list -> compute offset = dma_addr - GPU_BAR2_base
 *   -> dma_buf_unmap_attachment -> dma_buf_detach -> dma_buf_put
 *
 * Consumer flow (vmem_create_dmabuf):
 *   pfn_list -> alloc vmem_buf -> register dma_buf_ops -> dma_buf_export
 *   When XE driver later calls map_dma_buf:
 *     for each entry: dma_map_resource(phys = ep_bar2_base + offset)
 *     -> return sg_table with DMA addresses pointing into endpoint BAR2
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>

#include "vmem_dmabuf.h"

/* -- Producer: parse real dma-buf -------------------- */

/* Required non-NULL callback for dma_buf_dynamic_attach dynamic importer.
 * vmem only reads PFN offsets transiently; we don't handle buffer moves. */
static void vmem_move_notify(struct dma_buf_attachment *attach)
{
	/* Buffer moved by exporter; vmem ignores this as the import is short-lived */
}

static const struct dma_buf_attach_ops vmem_importer_ops = {
	.allow_peer2peer = true,
	.move_notify     = vmem_move_notify,  /* must not be NULL */
};

int vmem_parse_dmabuf(int dmabuf_fd,
		      u8 bus, u8 pci_devno, u8 fn,
		      struct vmem_pfn_list *out)
{
	struct pci_dev          *pdev = NULL;
	struct dma_buf          *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table         *sgt = NULL;
	struct scatterlist      *sg;
	phys_addr_t              bar2_base;
	unsigned int             i;
	int                      ret = 0;

	/* Look up the GPU PCI device by BDF */
	pdev = pci_get_domain_bus_and_slot(0, bus, PCI_DEVFN(pci_devno, fn));
	if (!pdev) {
		pr_err("vmem: GPU PCI device %02x:%02x.%x not found\n",
		       bus, pci_devno, fn);
		return -ENODEV;
	}

	/* BAR 2 is Intel Arc GPU LMEM (VRAM) window */
	bar2_base = pci_resource_start(pdev, 2);
	if (!bar2_base) {
		pr_err("vmem: GPU BAR2 not available for %02x:%02x.%x\n",
		       bus, pci_devno, fn);
		ret = -ENXIO;
		goto put_pdev;
	}
	pr_debug("vmem: GPU BAR2 base = 0x%llx\n", (u64)bar2_base);

	/* Get dma_buf from the fd */
	dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dmabuf)) {
		pr_err("vmem: dma_buf_get(fd=%d) failed: %ld\n",
		       dmabuf_fd, PTR_ERR(dmabuf));
		ret = PTR_ERR(dmabuf);
		goto put_pdev;
	}

	/*
	 * Intel XE driver requires DYNAMIC importers for LMEM (VRAM) dma-bufs.
	 * Static dma_buf_attach() is rejected with -EOPNOTSUPP for LMEM.
	 * Use dma_buf_dynamic_attach() + dma_buf_pin() to satisfy XE.
	 * NOTE: move_notify must be non-NULL; kernel validates it for dynamic attach.
	 */
	attach = dma_buf_dynamic_attach(dmabuf, &pdev->dev,
					&vmem_importer_ops, NULL);
	if (IS_ERR(attach)) {
		pr_err("vmem: dma_buf_dynamic_attach failed: %ld\n", PTR_ERR(attach));
		ret = PTR_ERR(attach);
		goto put_dmabuf;
	}

	/*
	 * dma_buf_map_attachment_unlocked() acquires the dma_resv lock and
	 * calls dma_buf_pin() internally for dynamic importers. Do NOT call
	 * dma_buf_pin() explicitly here (it requires the lock to be held).
	 * Similarly, dma_buf_unmap_attachment_unlocked() calls dma_buf_unpin().
	 */
	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		pr_err("vmem: dma_buf_map_attachment failed: %ld\n", PTR_ERR(sgt));
		ret = PTR_ERR(sgt);
		goto detach;
	}

	/* Walk the DMA scatter list, computing offsets from BAR2 base */
	out->count = 0;
	i = 0;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		dma_addr_t dma_addr = sg_dma_address(sg);
		u32        size     = (u32)sg_dma_len(sg);
		u64        offset;

		if (i >= VMEM_MAX_PFN_ENTRIES) {
			pr_warn("vmem: scatter list exceeds VMEM_MAX_PFN_ENTRIES (%d)\n",
				VMEM_MAX_PFN_ENTRIES);
			ret = -ENOSPC;
			goto unmap;
		}

		if (dma_addr < bar2_base) {
			pr_err("vmem: sg entry dma_addr 0x%llx < BAR2 base 0x%llx\n",
			       (u64)dma_addr, (u64)bar2_base);
			ret = -EINVAL;
			goto unmap;
		}

		offset = dma_addr - bar2_base;
		out->entries[i].offset = offset;
		out->entries[i].size   = size;
		out->entries[i].pad    = 0;

		pr_debug("vmem: sg[%u]: dma=0x%llx offset=0x%llx size=%u\n",
			 i, (u64)dma_addr, offset, size);
	}
	out->count = i;
	out->pad   = 0;
	pr_info("vmem: parsed %u scatter entries from fd=%d\n", i, dmabuf_fd);

unmap:
	/* dma_buf_unmap_attachment_unlocked calls dma_buf_unpin internally */
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(dmabuf, attach);
put_dmabuf:
	dma_buf_put(dmabuf);
put_pdev:
	pci_dev_put(pdev);
	return ret;
}

/* -- Consumer: fake dma-buf implementation -------------------- */

static struct sg_table *vmem_dmabuf_map(struct dma_buf_attachment *attach,
					enum dma_data_direction dir)
{
	struct vmem_buf   *vbuf = attach->dmabuf->priv;
	struct sg_table   *sgt;
	struct scatterlist *sg;
	unsigned int       i;
	int                ret;

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
		phys_addr_t phys = vbuf->ep_bar2_base + vbuf->entries[i].offset;
		size_t      sz   = vbuf->entries[i].size;
		dma_addr_t  dma;

		/*
		 * Map the physical BAR address as a device resource (not system
		 * RAM).  dma_map_resource() is the correct API for MMIO regions.
		 * Requires IOMMU passthrough for correct PCIe P2P operation.
		 */
		dma = dma_map_resource(attach->dev, phys, sz, dir,
				       DMA_ATTR_SKIP_CPU_SYNC);
		if (dma_mapping_error(attach->dev, dma)) {
			pr_err("vmem: dma_map_resource failed for entry %u (phys=0x%llx)\n",
			       i, (u64)phys);
			/* Unwind already-mapped entries */
			{ struct scatterlist *us = sgt->sgl; unsigned int j;
			for (j = 0; j < i; j++, us = sg_next(us))
				dma_unmap_resource(attach->dev, sg_dma_address(us),
						   sg_dma_len(us), dir, DMA_ATTR_SKIP_CPU_SYNC); }
			sg_free_table(sgt);
			kfree(sgt);
			return ERR_PTR(-ENOMEM);
		}

		/*
		 * For BAR (device) memory there are no backing struct pages.
		 * Set page_link = 0 so the entry is treated as a raw DMA addr.
		 * GPU drivers (like XE) only access sg_dma_address / sg_dma_len.
		 */
		sg->page_link = 0;
		sg->offset    = 0;
		sg->length    = sz;
		sg_dma_address(sg) = dma;
		sg_dma_len(sg)     = sz;

		pr_debug("vmem: map entry %u: phys=0x%llx dma=0x%llx size=%zu\n",
			 i, (u64)phys, (u64)dma, sz);
	}

	return sgt;
}

static void vmem_dmabuf_unmap(struct dma_buf_attachment *attach,
			      struct sg_table *sgt,
			      enum dma_data_direction dir)
{
	struct vmem_buf    *vbuf = attach->dmabuf->priv;
	struct scatterlist *sg;
	unsigned int        i;

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
	kfree(vbuf);
}

static int vmem_dmabuf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attach)
{
	/* Nothing special on attach; mapping done in map_dma_buf */
	return 0;
}

static void vmem_dmabuf_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attach)
{
	/* Nothing to do; resources freed in unmap_dma_buf */
}

/*
 * mmap support: CPU access to the remote GPU VRAM via endpoint BAR2.
 * Uses io_remap_pfn_range since BAR memory is not RAM.
 */
static int vmem_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct vmem_buf *vbuf = dmabuf->priv;
	unsigned long    vaddr = vma->vm_start;
	unsigned int     i;
	int              ret;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	for (i = 0; i < vbuf->nr_entries; i++) {
		phys_addr_t  phys  = vbuf->ep_bar2_base + vbuf->entries[i].offset;
		size_t       sz    = vbuf->entries[i].size;
		unsigned long pfn  = phys >> PAGE_SHIFT;

		ret = io_remap_pfn_range(vma, vaddr, pfn, sz, vma->vm_page_prot);
		if (ret) {
			pr_err("vmem: io_remap_pfn_range failed for entry %u\n", i);
			return ret;
		}
		vaddr += sz;
	}
	return 0;
}

static const struct dma_buf_ops vmem_dmabuf_ops = {
	.attach        = vmem_dmabuf_attach,
	.detach        = vmem_dmabuf_detach,
	.map_dma_buf   = vmem_dmabuf_map,
	.unmap_dma_buf = vmem_dmabuf_unmap,
	.release       = vmem_dmabuf_release,
	.mmap          = vmem_dmabuf_mmap,
};

struct dma_buf *vmem_create_dmabuf(const struct vmem_pfn_list *pfn_list,
				   phys_addr_t ep_bar2_base)
{
	struct vmem_buf              *vbuf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf               *dmabuf;
	size_t                        total = 0;
	unsigned int                  i;

	if (!pfn_list->count || pfn_list->count > VMEM_MAX_PFN_ENTRIES) {
		pr_err("vmem: invalid pfn_list count %u\n", pfn_list->count);
		return ERR_PTR(-EINVAL);
	}
	if (!ep_bar2_base) {
		pr_err("vmem: endpoint BAR2 base not set (call REGISTER_EP first)\n");
		return ERR_PTR(-ENODEV);
	}

	vbuf = kzalloc(sizeof(*vbuf), GFP_KERNEL);
	if (!vbuf)
		return ERR_PTR(-ENOMEM);

	vbuf->nr_entries   = pfn_list->count;
	vbuf->ep_bar2_base = ep_bar2_base;

	for (i = 0; i < pfn_list->count; i++) {
		vbuf->entries[i] = pfn_list->entries[i];
		total += pfn_list->entries[i].size;
		pr_debug("vmem: consumer entry %u: offset=0x%llx size=%u\n",
			 i, pfn_list->entries[i].offset, pfn_list->entries[i].size);
	}
	vbuf->total_size = total;

	exp_info.ops   = &vmem_dmabuf_ops;
	exp_info.size  = total;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv  = vbuf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		pr_err("vmem: dma_buf_export failed: %ld\n", PTR_ERR(dmabuf));
		kfree(vbuf);
		return dmabuf;
	}

	pr_info("vmem: created fake dma_buf: %u entries, %zu bytes total\n",
		pfn_list->count, total);
	return dmabuf;
}

