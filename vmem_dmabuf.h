/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VMEM_DMABUF_H
#define _VMEM_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include "include/vmem_ioctl.h"
#include "vmem_buffer.h"

/*
 * vmem_buf: private context for the MMIO-backed pseudo dma-buf (peer side).
 * entries[i].addr = absolute peer-side PA (TARGET PA, translated by daemon Layer A
 * via Astera COSMOS SDK before being passed to KMD)
 * entries[i].size = byte length
 */
struct vmem_buf {
	unsigned int          nr_entries;
	size_t                total_size;
	struct vmem_pfn_entry *entries;
};

/* vmem_import_pin: persistent dma-buf attachment on the origin side */
struct vmem_import_pin {
	struct list_head           link;
	struct dma_buf            *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table           *sgt;
	struct pci_dev            *pdev;
	int                        orig_fd;
	pid_t                      pid;
};

/* vmem_file_priv: per-vmem-fd state */
struct vmem_file_priv {
	struct list_head  import_pins;
	struct mutex      pin_lock;
	struct list_head  buffers;
	struct mutex      buf_lock;
};

/* ── Origin side ─────────────────────────────────────────── */

/*
 * vmem_open_dmabuf_fd - P2P attach, walk sg_table, return absolute PAs.
 *
 * KMD returns entries[i].addr = sg_dma_address(sg)  (absolute physical address).
 * UMD forwards abs PAs to daemon as-is (no offset computation).
 *
 * @page_size:  out: PAGE_SIZE (4096)
 * @total_size: out: total bytes across all chunks
 * @count:      in/out: capacity / actual chunk count (-ENOSPC retry)
 */
int  vmem_open_dmabuf_fd(struct vmem_file_priv *priv,
			 int dmabuf_fd, u8 bus, u8 pci_dev, u8 fn,
			 struct vmem_pfn_entry __user *entries_ptr,
			 u32 *count, u32 *page_size, u64 *total_size);
int  vmem_close_dmabuf(struct vmem_file_priv *priv, int orig_fd);
void vmem_import_pins_cleanup(struct vmem_file_priv *priv);

/* ── Peer side ───────────────────────────────────────────── */
/* entries[i].addr = pre-translated absolute PA from daemon Layer A (COSMOS SDK). */
int  vmem_get_dmabuf_fd(struct vmem_file_priv *priv,
			struct vmem_pfn_entry __user *entries_ptr,
			u32 count);
int  vmem_put_dmabuf(struct vmem_file_priv *priv, int exported_fd);
void vmem_buffers_cleanup(struct vmem_file_priv *priv);

#endif /* _VMEM_DMABUF_H */
