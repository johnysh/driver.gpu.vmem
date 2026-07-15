// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_dmabuf.c - vMem pseudo dma-buf builder (consumer/peer side)
 *
 * vmem_get_ipc_handle() is the single entry point:
 *   Input:  {node_id, gpu_id, BAR-relative scatter offsets}
 *   Output: pseudo dma-buf fd backed by MMIO window into origin GPU VRAM
 *
 * PA synthesis (Method 1, runs entirely in KMD):
 *   astera_lookup(node_id, gpu_id) -> (dbdf, bar, index)
 *   base = pci_resource_start(dbdf, bar) + index * 32 GB
 *   PA'[i] = base + entries[i].offset
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
#include "vmem_astera.h"

/* ==========================================================================
 * Pseudo dma-buf ops (MMIO-backed, no CPU cache needed)
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
		phys_addr_t phys = (phys_addr_t)vbuf->entries[i].offset; /* absolute PA */
		size_t sz = vbuf->entries[i].size;
		dma_addr_t dma;

		dma = dma_map_resource(attach->dev, phys, sz, dir,
				       DMA_ATTR_SKIP_CPU_SYNC);
		if (dma_mapping_error(attach->dev, dma)) {
			struct scatterlist *us = sgt->sgl;
			unsigned int j;

			pr_err("vmem: dma_map_resource failed entry %u phys=0x%llx\n",
			       i, (u64)phys);
			for (j = 0; j < i; j++, us = sg_next(us))
				dma_unmap_resource(attach->dev,
						   sg_dma_address(us),
						   sg_dma_len(us), dir,
						   DMA_ATTR_SKIP_CPU_SYNC);
			sg_free_table(sgt);
			kfree(sgt);
			return ERR_PTR(-EIO);
		}

		sg->page_link      = 0;
		sg->offset         = 0;
		sg->length         = sz;
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
				   dir, DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(sgt);
	kfree(sgt);
}

static void vmem_dmabuf_release(struct dma_buf *dmabuf)
{
	struct vmem_buf *vbuf = dmabuf->priv;

	pr_debug("vmem: releasing pseudo dma-buf (nr_entries=%u)\n",
		 vbuf->nr_entries);
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
		phys_addr_t phys = (phys_addr_t)vbuf->entries[i].offset;
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
	return 0; /* MMIO; no CPU cache flush needed */
}

static int vmem_dmabuf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction dir)
{
	return 0;
}

static const struct dma_buf_ops vmem_dmabuf_ops = {
	.attach           = vmem_dmabuf_attach,
	.detach           = vmem_dmabuf_detach,
	.map_dma_buf      = vmem_dmabuf_map,
	.unmap_dma_buf    = vmem_dmabuf_unmap,
	.release          = vmem_dmabuf_release,
	.mmap             = vmem_dmabuf_mmap,
	.begin_cpu_access = vmem_dmabuf_begin_cpu_access,
	.end_cpu_access   = vmem_dmabuf_end_cpu_access,
};

/* ==========================================================================
 * vmem_get_ipc_handle — Method 1: in-kernel PA synthesis via Astera vFE
 * ========================================================================== */

/**
 * vmem_get_ipc_handle - build pseudo dma-buf via in-kernel Astera vFE lookup
 *
 * Input:  {node_id, gpu_id} identify the origin GPU; entries[] are the
 *         BAR-relative scatter offsets forwarded from the origin node over
 *         the control channel.
 *
 * KMD flow:
 *   1. astera_lookup(node_id, gpu_id) -> ep = {dbdf, bar, index}
 *   2. base = pci_resource_start(dbdf, bar) + index * VMEM_VFE_WINDOW_SIZE
 *   3. PA'[i] = base + entries[i].offset
 *   4. export MMIO pseudo dma-buf -> fd for zeMemOpenIpcHandle on peer GPU
 */
struct dma_buf *vmem_get_ipc_handle(u32 node_id, u32 gpu_id,
				    struct vmem_pfn_entry __user *entries_ptr,
				    u32 count)
{
	struct astera_vfe_ep ep;
	struct pci_dev *pdev;
	phys_addr_t bar_start, base;
	struct vmem_pfn_entry *bar_entries, *pa_entries;
	struct vmem_buf *vbuf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	size_t total = 0;
	u32 i;
	int ret;

	if (!count) {
		pr_err("vmem: get_ipc_handle: count=0\n");
		return ERR_PTR(-EINVAL);
	}

	/* Step 1: resolve (node_id, gpu_id) -> vFE endpoint */
	ret = vmem_astera_lookup(node_id, gpu_id, &ep);
	if (ret) {
		pr_err("vmem: get_ipc_handle: astera_lookup(node=%u gpu=%u) -> %d\n",
		       node_id, gpu_id, ret);
		return ERR_PTR(ret);
	}

	/* Step 2: find the vFE PCIe device and compute window base */
	pdev = pci_get_domain_bus_and_slot(ep.domain, ep.bus, ep.devfn);
	if (!pdev) {
		pr_err("vmem: get_ipc_handle: vFE pdev not found "
		       "(%04x:%02x:%02x.%u)\n",
		       ep.domain, ep.bus,
		       PCI_SLOT(ep.devfn), PCI_FUNC(ep.devfn));
		return ERR_PTR(-ENODEV);
	}

	bar_start = pci_resource_start(pdev, ep.bar);
	pci_dev_put(pdev);
	if (!bar_start) {
		pr_err("vmem: get_ipc_handle: vFE BAR%u not mapped\n", ep.bar);
		return ERR_PTR(-ENXIO);
	}

	base = bar_start + (phys_addr_t)ep.index * VMEM_VFE_WINDOW_SIZE;
	pr_debug("vmem: get_ipc_handle: BAR%u=0x%llx idx=%u -> base=0x%llx\n",
		 ep.bar, (u64)bar_start, ep.index, (u64)base);

	/* Copy BAR-relative entries from userspace */
	bar_entries = kvcalloc(count, sizeof(*bar_entries), GFP_KERNEL);
	if (!bar_entries)
		return ERR_PTR(-ENOMEM);

	if (copy_from_user(bar_entries, entries_ptr,
			   count * sizeof(*bar_entries))) {
		kvfree(bar_entries);
		return ERR_PTR(-EFAULT);
	}

	/* Step 3: translate BAR-relative offsets -> absolute PAs */
	pa_entries = kvcalloc(count, sizeof(*pa_entries), GFP_KERNEL);
	if (!pa_entries) {
		kvfree(bar_entries);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < count; i++) {
		pa_entries[i].offset = (u64)(base + bar_entries[i].offset);
		pa_entries[i].size   = bar_entries[i].size;
		pa_entries[i].pad    = 0;
		total += bar_entries[i].size;
		pr_debug("vmem: pa[%u] = 0x%llx + 0x%llx = 0x%llx size=%u\n",
			 i, (u64)base, bar_entries[i].offset,
			 pa_entries[i].offset, pa_entries[i].size);
	}
	kvfree(bar_entries);

	/* Step 4: build MMIO pseudo dma-buf */
	vbuf = kzalloc(sizeof(*vbuf), GFP_KERNEL);
	if (!vbuf) {
		kvfree(pa_entries);
		return ERR_PTR(-ENOMEM);
	}
	vbuf->nr_entries = count;
	vbuf->total_size = total;
	vbuf->entries    = pa_entries; /* ownership transferred */

	exp_info.ops   = &vmem_dmabuf_ops;
	exp_info.size  = total;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv  = vbuf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		pr_err("vmem: get_ipc_handle: dma_buf_export failed: %ld\n",
		       PTR_ERR(dmabuf));
		kvfree(pa_entries);
		kfree(vbuf);
		return dmabuf;
	}

	pr_info("vmem: get_ipc_handle: %u entries node=%u gpu=%u base=0x%llx total=%zu\n",
		count, node_id, gpu_id, (u64)base, total);
	return dmabuf;
}

/* ==========================================================================
 * vmem_identify_dmabuf_source
 * ========================================================================== */

static void vmem_move_notify(struct dma_buf_attachment *attach)
{
	pr_warn("vmem: move_notify - buffer moved by exporter\n");
}

static const struct dma_buf_attach_ops vmem_importer_ops = {
	.allow_peer2peer = true,
	.move_notify     = vmem_move_notify,
};

int vmem_identify_dmabuf_source(int fd,
				u8 bus, u8 dev, u8 fn,
				u8 *src_bus, u8 *src_dev, u8 *src_fn)
{
	struct pci_dev *pdev, *iter = NULL;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct scatterlist *sg;
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
	first_phys = (phys_addr_t)sg_dma_address(sg);

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
