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
 * struct vmem_buf - internal context for a consumer-side fake dma-buf
 * @nr_entries:    number of scatter entries
 * @ep_bar2_base:  consumer GPU BAR2 physical base (0 when abs_pa)
 * @total_size:    total byte size of the buffer
 * @abs_pa:        true when entries[].offset are absolute physical addresses
 * @entries:       kvmalloc'd scatter entry array; freed in vmem_dmabuf_release
 */
struct vmem_buf {
	unsigned int          nr_entries;
	phys_addr_t           ep_bar2_base;
	size_t                total_size;
	bool                  abs_pa;
	struct vmem_pfn_entry *entries;
};

/**
 * struct vmem_import_pin - persistent dma-buf attachment preventing TTM eviction
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
 * struct vmem_file_priv - per-fd private data
 */
struct vmem_file_priv {
	struct list_head  import_pins;
	struct mutex      pin_lock;
};

/* Producer side */
int vmem_parse_dmabuf(struct vmem_file_priv *priv,
		      int dmabuf_fd,
		      u8 bus, u8 pci_dev, u8 fn,
		      u8 flags,
		      struct vmem_pfn_entry __user *entries_ptr,
		      u32 *count);

/* Consumer side */
struct dma_buf *vmem_create_dmabuf(struct vmem_pfn_entry __user *entries_ptr,
				   u32 count,
				   u8 consumer_bus, u8 consumer_dev,
				   u8 consumer_fn, u8 flags);

/* Source detection */
int vmem_identify_dmabuf_source(int fd,
				u8 bus, u8 dev, u8 fn,
				u8 *src_bus, u8 *src_dev, u8 *src_fn);

/* Lifecycle */
void vmem_import_pins_cleanup(struct vmem_file_priv *priv);
int  vmem_release_pin(struct vmem_file_priv *priv, int orig_fd);

#endif /* _VMEM_DMABUF_H */
