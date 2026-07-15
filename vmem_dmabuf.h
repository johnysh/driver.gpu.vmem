/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmem_dmabuf.h - vMem dma-buf operations (Method 1: KMD PA synthesis)
 *
 * Origin side:
 *   vmem_open_dmabuf_fd()   -- attach GPU dma-buf, extract BAR-relative offsets, soft-pin
 *   vmem_close_dmabuf()     -- release soft-pin (unmap -> detach -> put)
 *   vmem_import_pins_cleanup() -- sweep remaining pins on fd close / crash
 *
 * Peer side:
 *   vmem_get_dmabuf_fd()    -- astera_lookup -> PA synthesis -> MMIO pseudo dma-buf fd
 *   vmem_put_dmabuf()       -- remove tracking obj, close_fd, kref_put
 *   vmem_buffers_cleanup()  -- sweep remaining buffer objs on fd close / crash
 */
#ifndef _VMEM_DMABUF_H
#define _VMEM_DMABUF_H

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include "include/vmem_ioctl.h"
#include "vmem_buffer.h"

/* vmem_buf: private context for the pseudo dma-buf (MMIO-backed) */
struct vmem_buf {
	unsigned int          nr_entries;
	size_t                total_size;
	struct vmem_pfn_entry *entries;  /* entries[i].offset = absolute PA */
};

/* vmem_import_pin: persistent dma-buf attachment on the origin side */
struct vmem_import_pin {
	struct list_head           link;
	struct dma_buf            *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table           *sgt;
	struct pci_dev            *pdev;
	int                        orig_fd;  /* GPU dma-buf fd passed to OPEN_DMABUF */
	pid_t                      pid;      /* opener pid for debugfs */
};

/* vmem_file_priv: per-vmem-fd state */
struct vmem_file_priv {
	struct list_head  import_pins; /* origin soft-pins (vmem_import_pin) */
	struct mutex      pin_lock;
	struct list_head  buffers;     /* peer pseudo dma-bufs (vmem_buffer_obj) */
	struct mutex      buf_lock;
};

/* ── Origin side ─────────────────────────────────────────── */
int  vmem_open_dmabuf_fd(struct vmem_file_priv *priv,
			 int dmabuf_fd, u8 bus, u8 pci_dev, u8 fn,
			 struct vmem_pfn_entry __user *entries_ptr,
			 u32 *count);
int  vmem_close_dmabuf(struct vmem_file_priv *priv, int orig_fd);
void vmem_import_pins_cleanup(struct vmem_file_priv *priv);

/* ── Peer side ───────────────────────────────────────────── */
int  vmem_get_dmabuf_fd(struct vmem_file_priv *priv,
			u32 node_id, u32 gpu_id,
			struct vmem_pfn_entry __user *entries_ptr,
			u32 count);
int  vmem_put_dmabuf(struct vmem_file_priv *priv, int exported_fd);
void vmem_buffers_cleanup(struct vmem_file_priv *priv);

#endif /* _VMEM_DMABUF_H */
