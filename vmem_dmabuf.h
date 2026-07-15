/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VMEM_DMABUF_H
#define _VMEM_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/pci.h>
#include "include/vmem_ioctl.h"

/**
 * struct vmem_buf - internal context for a pseudo dma-buf (consumer side)
 * @nr_entries:  number of scatter entries
 * @total_size:  total byte size of the buffer
 * @entries:     kvmalloc'd array; entries[i].offset = absolute PA (vFE window)
 */
struct vmem_buf {
	unsigned int          nr_entries;
	size_t                total_size;
	struct vmem_pfn_entry *entries;
};

/**
 * vmem_get_ipc_handle - build a pseudo dma-buf via in-kernel Astera vFE lookup
 *
 * Implements VMEM_IOCTL_GET_IPC_HANDLE.
 *
 * KMD flow:
 *   1. astera_lookup(node_id, gpu_id) -> (dbdf, bar, index)
 *   2. base = pci_resource_start(dbdf, bar) + index * 32 GB
 *   3. PA'[i] = base + entries[i].offset
 *   4. build MMIO pseudo dma-buf from PA' array
 *
 * @node_id:     remote node identifier
 * @gpu_id:      remote GPU identifier on that node
 * @entries_ptr: userspace ptr -> vmem_pfn_entry[] (BAR-relative offsets)
 * @count:       number of scatter entries
 *
 * Returns pseudo dma-buf on success, ERR_PTR on failure:
 *   -ENOSYS  Astera vFE driver not registered
 *   -ENODEV  (node_id, gpu_id) not in vFE endpoint table
 *   -ENXIO   vFE BAR not mapped
 */
struct dma_buf *vmem_get_ipc_handle(u32 node_id, u32 gpu_id,
				    struct vmem_pfn_entry __user *entries_ptr,
				    u32 count);

/* Source detection utility */
int vmem_identify_dmabuf_source(int fd,
				u8 bus, u8 dev, u8 fn,
				u8 *src_bus, u8 *src_dev, u8 *src_fn);

#endif /* _VMEM_DMABUF_H */
