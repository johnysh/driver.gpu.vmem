/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmem_dmabuf.h - internal header for vMem dma-buf operations
 */

#ifndef _VMEM_DMABUF_H
#define _VMEM_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include "include/vmem_ioctl.h"

/**
 * struct vmem_buf - internal context for a synthetic (fake) dma-buf
 *
 * Created by vmem_create_dmabuf() on the consumer side.  Lives until the
 * dma_buf refcount reaches zero (vmem_dmabuf_release is called).
 *
 * @nr_entries:    number of PFN scatter entries
 * @ep_bar2_base:  local PCIe endpoint BAR2 physical base address
 * @total_size:    sum of all entry sizes (bytes)
 * @entries:       copy of the received vmem_pfn_entry array
 */
struct vmem_buf {
	unsigned int        nr_entries;
	phys_addr_t         ep_bar2_base;
	size_t              total_size;
	struct vmem_pfn_entry entries[VMEM_MAX_PFN_ENTRIES];
};

/**
 * vmem_parse_dmabuf() - Producer: parse real dma-buf, extract PFN offsets
 *
 * Attaches to the dma-buf identified by @dmabuf_fd using the GPU device
 * specified by @bus/@dev/@fn, maps the scatter-gather list, computes
 * offset = sg_dma_address(sg) - GPU_BAR2_start for each scatter entry,
 * and populates @out.
 *
 * Caller must ensure IOMMU is in passthrough mode so that dma addresses
 * equal physical addresses (required for cross-node PCIe P2P).
 *
 * Returns 0 on success, negative errno on failure.
 */
int vmem_parse_dmabuf(int dmabuf_fd,
		      u8 bus, u8 pci_dev, u8 fn,
		      struct vmem_pfn_list *out);

/**
 * vmem_create_dmabuf() - Consumer: build fake dma-buf from PFN offset list
 *
 * For each entry in @pfn_list computes:
 *   physical_addr = @ep_bar2_base + entry.offset
 * then calls dma_map_resource() when attached and exports as a dma_buf.
 *
 * Returns a new struct dma_buf * on success, ERR_PTR on failure.
 * The caller must call dma_buf_put() or close the fd to release it.
 */
struct dma_buf *vmem_create_dmabuf(const struct vmem_pfn_list *pfn_list,
				   phys_addr_t ep_bar2_base);

#endif /* _VMEM_DMABUF_H */
