// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_dmabuf.c - vMem dma-buf parse (origin) and assemble (peer)
 *
 * v2.0 changes:
 *   - vmem_parse_dmabuf renamed to vmem_open_ipc_handle (same logic)
 *   - vmem_release_pin renamed to vmem_close_ipc_handle (same logic)
 *   - vmem_create_dmabuf refactored into:
 *       vmem_create_dmabuf_from_kpa()   internal, kernel-space PA array
 *   - vmem_get_ipc_handle_method1() added (Method 1: in-kernel PA synthesis):
 *       calls vmem_astera_lookup(node_id, gpu_id) -> vFE endpoint
 *       base = BAR_start + index * 32 GiB
 *       PA'[i] = base + bar_entries[i].offset
 *       delegates to vmem_create_dmabuf_from_kpa()
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
 * Common importer ops
 * ========================================================================== */

static void vmem_move_notify(struct dma_buf_attachment *attach)
{
	pr_warn("vmem: move_notify - buffer moved by exporter\n");
}

static const struct dma_buf_attach_ops vmem_importer_ops = {
	.allow_peer2peer = true,
	.move_notify     = vmem_move_notify,
};

/* ==========================================================================
 * Origin: vmem_open_ipc_handle
 * ========================================================================== */

/**
 * vmem_open_ipc_handle - attach to GPU dma-buf and extract scatter offsets
 *
 * Keeps the attachment alive (soft-pin) so XE TTM cannot evict the VRAM BO
 * while the peer holds the offset list.  Caller releases via
 * vmem_close_ipc_handle() (CLOSE_IPC_HANDLE ioctl) or vmem fd close.
 */
int vmem_open_ipc_handle(struct vmem_file_priv *priv,
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

	dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto put_pdev;
	}

	attach = dma_buf_dynamic_attach(dmabuf, &pdev->dev,
					&vmem_importer_ops, NULL);
	if (IS_ERR(attach)) {
		pr_err("vmem: dma_buf_dynamic_attach failed: %ld\n",
		       PTR_ERR(attach));
		ret = PTR_ERR(attach);
		goto put_dmabuf;
	}

	/*
	 * TODO: hard-pin via dma_buf_pin() when xe >= 6.19 is the minimum.
	 * Until then, keeping attach + sgt alive (soft-pin) prevents eviction.
	 */

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		pr_err("vmem: dma_buf_map_attachment failed: %ld\n",
		       PTR_ERR(sgt));
		ret = PTR_ERR(sgt);
		goto detach;
	}

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

		kentries[i].offset = (u64)(dma_addr - bar2_base);
		kentries[i].size   = size;
		kentries[i].pad    = 0;
	}

	pr_info("vmem: opened ipc handle: %u BAR-relative entries fd=%d bar2=0x%llx\n",
		nr_entries, dmabuf_fd, (u64)bar2_base);

	if (copy_to_user(entries_ptr, kentries,
			 nr_entries * sizeof(*kentries))) {
		ret = -EFAULT;
		goto free_kentries;
	}

	kvfree(kentries);
	kentries = NULL;

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
			pr_info("vmem: soft-pin stored for fd=%d\n", dmabuf_fd);
			return 0;
		}
		pr_warn("vmem: kzalloc(pin) failed; releasing mapping immediately\n");
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
 * Peer: fake dma-buf ops
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
		phys_addr_t phys = (phys_addr_t)vbuf->entries[i].offset;
		size_t sz  = vbuf->entries[i].size;
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
				   dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
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
	return 0;
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

/* ==========================================================================
 * Internal: build pseudo dma-buf from kernel-space absolute PA array
 * ========================================================================== */

static struct dma_buf *vmem_create_dmabuf_from_kpa(struct vmem_pfn_entry *kentries,
						   u32 count)
{
	struct vmem_buf *vbuf;
	struct vmem_pfn_entry *entries_copy;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	size_t total = 0;
	unsigned int i;

	entries_copy = kvcalloc(count, sizeof(*entries_copy), GFP_KERNEL);
	if (!entries_copy)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < count; i++) {
		entries_copy[i] = kentries[i];
		total += kentries[i].size;
	}

	vbuf = kzalloc(sizeof(*vbuf), GFP_KERNEL);
	if (!vbuf) {
		kvfree(entries_copy);
		return ERR_PTR(-ENOMEM);
	}

	vbuf->nr_entries = count;
	vbuf->total_size = total;
	vbuf->entries    = entries_copy;

	exp_info.ops   = &vmem_dmabuf_ops;
	exp_info.size  = total;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv  = vbuf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		pr_err("vmem: dma_buf_export failed: %ld\n", PTR_ERR(dmabuf));
		kvfree(entries_copy);
		kfree(vbuf);
	}
	return dmabuf;
}

/* ==========================================================================
 * Peer: vmem_get_ipc_handle_method1 - Method 1: in-kernel PA synthesis
 * ========================================================================== */

/**
 * vmem_get_ipc_handle_method1 - build pseudo dma-buf using in-kernel PA synthesis
 *
 * PA synthesis path (Method 1, runs entirely in KMD):
 *   1. vmem_astera_lookup(node_id, gpu_id) -> ep = {domain, bus, devfn, bar, index}
 *   2. vfe_pdev = pci_get_domain_bus_and_slot(ep.domain, ep.bus, ep.devfn)
 *   3. base = pci_resource_start(vfe_pdev, ep.bar) + ep.index * VMEM_VFE_WINDOW_SIZE
 *   4. PA'[i] = base + bar_entries[i].offset  (BAR-relative -> absolute PA)
 *   5. vmem_create_dmabuf_from_kpa(PA' array) -> pseudo dma-buf for xe import
 */
struct dma_buf *vmem_get_ipc_handle_method1(u32 node_id, u32 gpu_id,
					    struct vmem_pfn_entry __user *entries_ptr,
					    u32 count)
{
	struct astera_vfe_ep ep;
	struct pci_dev *vfe_pdev;
	phys_addr_t bar_start, base;
	struct vmem_pfn_entry *bar_entries, *pa_entries;
	struct dma_buf *dmabuf;
	u32 i;
	int ret;

	if (!count) {
		pr_err("vmem: get_ipc_handle_method1: count=0\n");
		return ERR_PTR(-EINVAL);
	}

	/* Step 1: resolve (node_id, gpu_id) -> vFE endpoint */
	ret = vmem_astera_lookup(node_id, gpu_id, &ep);
	if (ret) {
		pr_err("vmem: method1: astera_lookup(node=%u gpu=%u) failed: %d\n",
		       node_id, gpu_id, ret);
		return ERR_PTR(ret);
	}

	/* Step 2: find the vFE PCIe device */
	vfe_pdev = pci_get_domain_bus_and_slot(ep.domain, ep.bus, ep.devfn);
	if (!vfe_pdev) {
		pr_err("vmem: method1: vFE pdev not found (%04x:%02x:%02x.%u)\n",
		       ep.domain, ep.bus,
		       PCI_SLOT(ep.devfn), PCI_FUNC(ep.devfn));
		return ERR_PTR(-ENODEV);
	}

	/* Step 3: compute MMIO window base */
	bar_start = pci_resource_start(vfe_pdev, ep.bar);
	pci_dev_put(vfe_pdev);
	if (!bar_start) {
		pr_err("vmem: method1: vFE BAR%u not mapped\n", ep.bar);
		return ERR_PTR(-ENXIO);
	}
	base = bar_start + (phys_addr_t)ep.index * VMEM_VFE_WINDOW_SIZE;
	pr_debug("vmem: method1: BAR%u=0x%llx + idx=%u*32G -> base=0x%llx\n",
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

	/* Step 4: translate BAR-relative offsets -> absolute PAs */
	pa_entries = kvcalloc(count, sizeof(*pa_entries), GFP_KERNEL);
	if (!pa_entries) {
		kvfree(bar_entries);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < count; i++) {
		pa_entries[i].offset = (u64)(base + bar_entries[i].offset);
		pa_entries[i].size   = bar_entries[i].size;
		pa_entries[i].pad    = 0;
		pr_debug("vmem: method1: pa[%u] = 0x%llx + 0x%llx = 0x%llx size=%u\n",
			 i, (u64)base, bar_entries[i].offset,
			 pa_entries[i].offset, pa_entries[i].size);
	}
	kvfree(bar_entries);

	pr_info("vmem: method1: %u entries node=%u gpu=%u base=0x%llx\n",
		count, node_id, gpu_id, (u64)base);

	/* Step 5: build MMIO pseudo dma-buf */
	dmabuf = vmem_create_dmabuf_from_kpa(pa_entries, count);
	kvfree(pa_entries);

	return dmabuf;
}

/* ==========================================================================
 * Source detection: vmem_identify_dmabuf_source
 * ========================================================================== */

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
 * Origin lifecycle: pin management
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

int vmem_close_ipc_handle(struct vmem_file_priv *priv, int orig_fd)
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
	pr_debug("vmem: closed ipc handle orig_fd=%d\n", orig_fd);
	kfree(found);
	return 0;
}
