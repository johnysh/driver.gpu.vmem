/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VMEM_DMABUF_H
#define _VMEM_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include "include/vmem_ioctl.h"

/**
 * struct vmem_buf - internal context for a peer-side pseudo dma-buf
 * @nr_entries:  number of scatter entries
 * @total_size:  total byte size of the buffer
 * @entries:     kvmalloc'd array; entries[i].offset = absolute PA (MMIO window)
 */
struct vmem_buf {
	unsigned int          nr_entries;
	size_t                total_size;
	struct vmem_pfn_entry *entries;
};

/**
 * struct vmem_import_pin - persistent dma-buf attachment (soft-pin)
 * Stored per vmem fd on the origin side; released by CLOSE_IPC_HANDLE or fd close.
 */
struct vmem_import_pin {
	struct list_head           link;
	struct dma_buf            *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table           *sgt;
	struct pci_dev            *pdev;
	int                        orig_fd;
};

/**
 * struct vmem_file_priv - per-fd private state
 */
struct vmem_file_priv {
	struct list_head  import_pins;  /* origin-side persistent soft-pins */
	struct mutex      pin_lock;
};

/* ── Origin side ─────────────────────────────────────────── */

/**
 * vmem_open_ipc_handle - attach GPU dma-buf and extract BAR-relative offsets
 *
 * Implements VMEM_IOCTL_OPEN_IPC_HANDLE.
 * Stores a soft-pin until vmem_close_ipc_handle() or fd close.
 */
int vmem_open_ipc_handle(struct vmem_file_priv *priv,
			 int dmabuf_fd,
			 u8 bus, u8 pci_dev, u8 fn,
			 struct vmem_pfn_entry __user *entries_ptr,
			 u32 *count);

/* ── Peer side ───────────────────────────────────────────── */

/**
 * vmem_get_ipc_handle_method1 - Method 1: in-kernel PA synthesis via Astera vFE
 *
 * Implements VMEM_IOCTL_GET_IPC_HANDLE.
 *
 * Steps executed in KMD:
 *   1. vmem_astera_lookup(node_id, gpu_id) -> vFE endpoint (domain/bus/devfn/bar/index)
 *   2. base = pci_resource_start(vfe_pdev, ep.bar) + ep.index * VMEM_VFE_WINDOW_SIZE
 *   3. PA'[i] = base + entries[i].offset  (BAR-relative -> absolute PA)
 *   4. Build MMIO pseudo dma-buf from PA' array
 *
 * Returns the pseudo dma-buf on success, ERR_PTR on failure:
 *   -ENOSYS  Astera vFE driver not registered
 *   -ENODEV  (node_id, gpu_id) not found in vFE table
 *   -ENXIO   vFE BAR not mapped
 */
struct dma_buf *vmem_get_ipc_handle_method1(u32 node_id, u32 gpu_id,
					    struct vmem_pfn_entry __user *entries_ptr,
					    u32 count);

/* ── Source detection ────────────────────────────────────── */
int vmem_identify_dmabuf_source(int fd,
				u8 bus, u8 dev, u8 fn,
				u8 *src_bus, u8 *src_dev, u8 *src_fn);

/* ── Origin lifecycle ────────────────────────────────────── */
void vmem_import_pins_cleanup(struct vmem_file_priv *priv);
int  vmem_close_ipc_handle(struct vmem_file_priv *priv, int orig_fd);

/* Backward compat alias */
#define vmem_release_pin  vmem_close_ipc_handle

#endif /* _VMEM_DMABUF_H */
