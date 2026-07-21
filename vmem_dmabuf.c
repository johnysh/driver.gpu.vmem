// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_dmabuf.c - vMem dma-buf operations (Method 2: pass-through)
 *
 * KMD and UMD do NOT perform address translation. The IMEMLINK daemon
 * (Layer A) calls the Astera COSMOS SDK in userspace to translate origin GPU
 * BAR-relative offsets to TARGET physical addresses, then passes the result
 * down to the peer UMD, which forwards them verbatim to KMD.
 *
 * Origin side:
 *   vmem_open_dmabuf_fd()   -- P2P attach, walk sg_table, return ABSOLUTE PAs.
 *                              UMD forwards abs PAs to daemon as-is.
 *   vmem_close_dmabuf()     -- release soft-pin: unmap -> detach -> put
 *   vmem_import_pins_cleanup() -- backstop sweep on vmem fd close / crash
 *
 * Peer side (Method 2: pass-through, translation done by daemon via COSMOS SDK):
 *   vmem_get_dmabuf_fd()    -- take pre-translated abs PAs from UMD directly
 *                              -> MMIO pseudo dma-buf fd (no astera_lookup)
 *   vmem_put_dmabuf()       -- remove vmem_buffer_obj, close_fd, kref_put
 *   vmem_buffers_cleanup()  -- backstop sweep of remaining exported bufs
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/fdtable.h>

#include "vmem_dmabuf.h"
#include "vmem_buffer.h"
#include "vmem_debugfs.h"

/* ==========================================================================
 * Common importer ops (origin side P2P attach)
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
 * Origin: vmem_open_dmabuf_fd
 * ========================================================================== */

/**
 * vmem_open_dmabuf_fd - attach to GPU dma-buf and return absolute physical addresses
 *
 * Implements VMEM_IOCTL_OPEN_DMABUF.
 *
 * KMD flow (§5.1):
 *   dma_buf_get(fd)  ->  dynamic P2P attach (allow_peer2peer=true, owning GPU)
 *   dma_buf_map_attachment()  ->  sg_table
 *   For each sg entry: sg_dma_address() == PA  (IOMMU-off assumption)
 *   Returns {abs PA, size} per chunk in entries[].addr/size
 *   Keeps attachment + sg_table alive as soft-pin.
 *
 * Daemon A responsibility after this call:
 *   bar2_base = sysfs read or COSMOS SDK
 *   pa_offset[i] = entries[i].addr - bar2_base  <- stored in BlobStore
 *
 * Two-step count negotiation:
 *   count=0  -> -ENOSPC, count=required N
 *   count=N  -> fills entries[], page_size=PAGE_SIZE, total_size=sum(size[i])
 */
int vmem_open_dmabuf_fd(struct vmem_file_priv *priv,
			int dmabuf_fd, u8 bus, u8 pci_devno, u8 fn,
			struct vmem_pfn_entry __user *entries_ptr,
			u32 *count, u32 *page_size, u64 *total_size)
{
	struct pci_dev *pdev = NULL;
	struct dma_buf *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;
	struct vmem_pfn_entry *kentries = NULL;
	struct vmem_seg *dbg_segs = NULL;
	u32 capacity = *count;
	u32 nr_entries;
	u64 total = 0;
	struct scatterlist *sg;
	int i, ret = 0;

	/* GPU pdev is needed only for the P2P attach device; BAR2 is NOT read here */
	pdev = pci_get_domain_bus_and_slot(0, bus, PCI_DEVFN(pci_devno, fn));
	if (!pdev) {
		pr_err("vmem: open_dmabuf: GPU %02x:%02x.%x not found\n",
		       bus, pci_devno, fn);
		return -ENODEV;
	}

	dmabuf = dma_buf_get(dmabuf_fd);
	if (IS_ERR(dmabuf)) { ret = PTR_ERR(dmabuf); goto put_pdev; }

	attach = dma_buf_dynamic_attach(dmabuf, &pdev->dev,
					&vmem_importer_ops, NULL);
	if (IS_ERR(attach)) {
		pr_err("vmem: open_dmabuf: dma_buf_dynamic_attach failed: %ld\n",
		       PTR_ERR(attach));
		ret = PTR_ERR(attach); goto put_dmabuf;
	}

	/*
	 * TODO: hard-pin via dma_buf_pin() when xe >= 6.19 is minimum.
	 * Until then, keeping attach + sgt alive is the soft-pin.
	 */
	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		pr_err("vmem: open_dmabuf: dma_buf_map_attachment failed: %ld\n",
		       PTR_ERR(sgt));
		ret = PTR_ERR(sgt); goto detach;
	}

	nr_entries = sgt->nents;
	*count = nr_entries;
	if (capacity < nr_entries) { ret = -ENOSPC; goto unmap; }

	kentries = kvcalloc(nr_entries, sizeof(*kentries), GFP_KERNEL);
	dbg_segs  = kvcalloc(nr_entries, sizeof(*dbg_segs),  GFP_KERNEL);
	if (!kentries || !dbg_segs) { ret = -ENOMEM; goto free_tmp; }

	i = 0;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		/*
		 * IOMMU-off: sg_dma_address() == CPU physical address.
		 * Return absolute PA to UMD; UMD computes
		 *   offset = abs_pa - gpu_bar2_base
		 * and forwards offset list to peer.
		 */
		dma_addr_t abs_pa = sg_dma_address(sg);
		u32 sz = sg_dma_len(sg);

		kentries[i].addr = (u64)abs_pa;
		kentries[i].size = sz;
		dbg_segs[i].base = (phys_addr_t)abs_pa;
		dbg_segs[i].len  = sz;
		total += sz;
	}

	if (copy_to_user(entries_ptr, kentries, nr_entries * sizeof(*kentries))) {
		ret = -EFAULT; goto free_tmp;
	}

	kvfree(kentries);
	*page_size  = PAGE_SIZE;
	*total_size = total;

	/* Store soft-pin */
	{
		struct vmem_import_pin *pin = kzalloc(sizeof(*pin), GFP_KERNEL);
		if (!pin) { ret = -ENOMEM; goto unmap; }
		pin->dmabuf  = dmabuf;
		pin->attach  = attach;
		pin->sgt     = sgt;
		pin->pdev    = pdev;
		pin->orig_fd = dmabuf_fd;
		pin->pid     = task_pid_nr(current);
		mutex_lock(&priv->pin_lock);
		list_add_tail(&pin->link, &priv->import_pins);
		mutex_unlock(&priv->pin_lock);

		vmem_debugfs_add_imported(pin->pid, dmabuf_fd, total,
					  dbg_segs, nr_entries);
		kvfree(dbg_segs);
		pr_info("vmem: open_dmabuf: fd=%d %u chunks total=0x%llx (abs PAs returned)\n",
			dmabuf_fd, nr_entries, total);
		return 0;
	}

free_tmp:
	kvfree(kentries);
	kvfree(dbg_segs);
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
 * Origin: pin lifecycle
 * ========================================================================== */

void vmem_import_pins_cleanup(struct vmem_file_priv *priv)
{
	struct vmem_import_pin *pin, *tmp;

	mutex_lock(&priv->pin_lock);
	list_for_each_entry_safe(pin, tmp, &priv->import_pins, link) {
		list_del(&pin->link);
		vmem_debugfs_del_imported(pin->pid, pin->orig_fd);
		dma_buf_unmap_attachment_unlocked(pin->attach, pin->sgt, DMA_BIDIRECTIONAL);
		dma_buf_detach(pin->dmabuf, pin->attach);
		dma_buf_put(pin->dmabuf);
		pci_dev_put(pin->pdev);
		pr_debug("vmem: pin backstop orig_fd=%d\n", pin->orig_fd);
		kfree(pin);
	}
	mutex_unlock(&priv->pin_lock);
}

int vmem_close_dmabuf(struct vmem_file_priv *priv, int orig_fd)
{
	struct vmem_import_pin *pin, *tmp, *found = NULL;

	mutex_lock(&priv->pin_lock);
	list_for_each_entry_safe(pin, tmp, &priv->import_pins, link) {
		if (pin->orig_fd == orig_fd) {
			list_del(&pin->link);
			found = pin;
			break;
		}
	}
	mutex_unlock(&priv->pin_lock);
	if (!found) return -ENOENT;

	vmem_debugfs_del_imported(found->pid, orig_fd);
	dma_buf_unmap_attachment_unlocked(found->attach, found->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(found->dmabuf, found->attach);
	dma_buf_put(found->dmabuf);
	pci_dev_put(found->pdev);
	pr_info("vmem: close_dmabuf: soft-pin released orig_fd=%d\n", orig_fd);
	kfree(found);
	return 0;
}

/* ==========================================================================
 * Peer: pseudo dma-buf ops (MMIO-backed)
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
	if (!sgt) return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, vbuf->nr_entries, GFP_KERNEL);
	if (ret) { kfree(sgt); return ERR_PTR(ret); }

	sg = sgt->sgl;
	for (i = 0; i < vbuf->nr_entries; i++, sg = sg_next(sg)) {
		/* entries[i].addr holds absolute peer-side PA (vFE MMIO window) */
		phys_addr_t phys = (phys_addr_t)vbuf->entries[i].addr;
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
				dma_unmap_resource(attach->dev, sg_dma_address(us),
						   sg_dma_len(us), dir,
						   DMA_ATTR_SKIP_CPU_SYNC);
			sg_free_table(sgt);
			kfree(sgt);
			return ERR_PTR(-EIO);
		}
		sg->page_link = 0; sg->offset = 0; sg->length = sz;
		sg_dma_address(sg) = dma;
		sg_dma_len(sg)     = sz;
	}
	return sgt;
}

static void vmem_dmabuf_unmap(struct dma_buf_attachment *attach,
			      struct sg_table *sgt, enum dma_data_direction dir)
{
	struct vmem_buf *vbuf = attach->dmabuf->priv;
	struct scatterlist *sg;
	unsigned int i;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (i >= vbuf->nr_entries) break;
		dma_unmap_resource(attach->dev, sg_dma_address(sg),
				   vbuf->entries[i].size, dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(sgt);
	kfree(sgt);
}

static void vmem_dmabuf_release(struct dma_buf *dmabuf)
{
	struct vmem_buf *vbuf = dmabuf->priv;
	pr_debug("vmem: releasing pseudo dma-buf (nr_entries=%u)\n", vbuf->nr_entries);
	kvfree(vbuf->entries);
	kfree(vbuf);
}

static int  vmem_dmabuf_attach(struct dma_buf *d, struct dma_buf_attachment *a) { return 0; }
static void vmem_dmabuf_detach(struct dma_buf *d, struct dma_buf_attachment *a) {}

static int vmem_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct vmem_buf *vbuf = dmabuf->priv;
	unsigned long vaddr = vma->vm_start;
	unsigned int i;
	int ret;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	for (i = 0; i < vbuf->nr_entries; i++) {
		ret = io_remap_pfn_range(vma, vaddr,
					 (phys_addr_t)vbuf->entries[i].addr >> PAGE_SHIFT,
					 vbuf->entries[i].size, vma->vm_page_prot);
		if (ret) { pr_err("vmem: io_remap_pfn_range failed entry %u\n", i); return ret; }
		vaddr += vbuf->entries[i].size;
	}
	return 0;
}

static int vmem_dmabuf_begin_cpu(struct dma_buf *d, enum dma_data_direction dir) { return 0; }
static int vmem_dmabuf_end_cpu  (struct dma_buf *d, enum dma_data_direction dir) { return 0; }

static const struct dma_buf_ops vmem_dmabuf_ops = {
	.attach           = vmem_dmabuf_attach,
	.detach           = vmem_dmabuf_detach,
	.map_dma_buf      = vmem_dmabuf_map,
	.unmap_dma_buf    = vmem_dmabuf_unmap,
	.release          = vmem_dmabuf_release,
	.mmap             = vmem_dmabuf_mmap,
	.begin_cpu_access = vmem_dmabuf_begin_cpu,
	.end_cpu_access   = vmem_dmabuf_end_cpu,
};

/* ==========================================================================
 * Peer: vmem_get_dmabuf_fd  (Method 2: pass-through)
 * ========================================================================== */

/**
 * vmem_get_dmabuf_fd - build pseudo dma-buf from pre-translated absolute PAs.
 *
 * Implements VMEM_IOCTL_GET_DMABUF (Method 2).
 *
 * The IMEMLINK daemon (Layer A) has already performed address translation via
 * the Astera COSMOS SDK:
 *   cosmos_query_vfe(node_id, gpu_id) -> {vfe_dbdf, bar_id, slot_index}
 *   vfe_base = pci_resource_start(vfe_dbdf, bar_id) + slot_index * 32 GiB
 *   PA'[i]   = vfe_base + bar_offset[i]
 *
 * UMD passes these final absolute PAs directly. KMD does NOT call astera_lookup.
 *
 * @priv:        per-file state
 * @entries_ptr: userspace ptr -> vmem_pfn_entry[] with pre-translated abs PAs
 * @count:       number of entries
 * Returns: pseudo dma-buf fd (>= 0) or negative errno.
 */
int vmem_get_dmabuf_fd(struct vmem_file_priv *priv,
		       struct vmem_pfn_entry __user *entries_ptr,
		       u32 count)
{
	struct vmem_pfn_entry *pa_entries;
	struct vmem_buf *vbuf;
	struct vmem_buffer_obj *obj;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	size_t total = 0;
	u32 i;
	int fd;

	if (!count)
		return -EINVAL;

	/* Copy pre-translated absolute PAs from userspace */
	pa_entries = kvcalloc(count, sizeof(*pa_entries), GFP_KERNEL);
	if (!pa_entries)
		return -ENOMEM;
	if (copy_from_user(pa_entries, entries_ptr, count * sizeof(*pa_entries))) {
		kvfree(pa_entries);
		return -EFAULT;
	}

	for (i = 0; i < count; i++) {
		total += pa_entries[i].size;
		pr_debug("vmem: get_dmabuf: pa[%u] = 0x%llx size=%u\n",
			 i, pa_entries[i].addr, pa_entries[i].size);
	}

	/* Build MMIO pseudo dma-buf from pre-translated PAs */
	vbuf = kzalloc(sizeof(*vbuf), GFP_KERNEL);
	if (!vbuf) {
		kvfree(pa_entries);
		return -ENOMEM;
	}
	vbuf->nr_entries = count;
	vbuf->total_size = total;
	vbuf->entries    = pa_entries;

	exp_info.ops   = &vmem_dmabuf_ops;
	exp_info.size  = total;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv  = vbuf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		pr_err("vmem: get_dmabuf: dma_buf_export failed: %ld\n", PTR_ERR(dmabuf));
		kvfree(pa_entries);
		kfree(vbuf);
		return PTR_ERR(dmabuf);
	}

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		return fd;
	}

	/* Track with vmem_buffer_obj */
	obj = vmem_buffer_obj_alloc(count, total);
	if (!obj) {
		pr_warn("vmem: get_dmabuf: vmem_buffer_obj_alloc failed, fd=%d leaks\n", fd);
		goto done;
	}
	obj->dmabuf_fd = fd;
	obj->pid       = task_pid_nr(current);
	for (i = 0; i < count; i++) {
		obj->segs[i].base = (phys_addr_t)pa_entries[i].addr;
		obj->segs[i].len  = pa_entries[i].size;
	}
	mutex_lock(&priv->buf_lock);
	list_add_tail(&obj->link, &priv->buffers);
	mutex_unlock(&priv->buf_lock);
	vmem_debugfs_add_exported(obj->pid, fd, total, obj->segs, count);

done:
	pr_info("vmem: get_dmabuf: fd=%d %u entries total=0x%zx (pass-through, no Astera lookup)\n",
		fd, count, total);
	return fd;
}


/* ==========================================================================
 * Peer: vmem_put_dmabuf
 * ========================================================================== */

int vmem_put_dmabuf(struct vmem_file_priv *priv, int exported_fd)
{
	struct vmem_buffer_obj *obj, *tmp, *found = NULL;

	mutex_lock(&priv->buf_lock);
	list_for_each_entry_safe(obj, tmp, &priv->buffers, link) {
		if (obj->dmabuf_fd == exported_fd) {
			list_del(&obj->link);
			found = obj;
			break;
		}
	}
	mutex_unlock(&priv->buf_lock);
	if (!found) return -ENOENT;

	vmem_debugfs_del_exported(found->pid, exported_fd);
	close_fd(exported_fd);
	vmem_buffer_obj_put(found);
	pr_info("vmem: put_dmabuf: fd=%d released\n", exported_fd);
	return 0;
}

/* ==========================================================================
 * Peer: vmem_buffers_cleanup  (backstop)
 * ========================================================================== */

void vmem_buffers_cleanup(struct vmem_file_priv *priv)
{
	struct vmem_buffer_obj *obj, *tmp;

	mutex_lock(&priv->buf_lock);
	list_for_each_entry_safe(obj, tmp, &priv->buffers, link) {
		list_del(&obj->link);
		vmem_debugfs_del_exported(obj->pid, obj->dmabuf_fd);
		close_fd(obj->dmabuf_fd);
		vmem_buffer_obj_put(obj);
		pr_debug("vmem: buffer backstop fd=%d\n", obj->dmabuf_fd);
	}
	mutex_unlock(&priv->buf_lock);
}
