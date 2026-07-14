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
 * @total_size:    total byte size of the buffer
 * @entries:       kvmalloc'd array; entries[i].offset = absolute PA (from daemon)
 */
struct vmem_buf {
	unsigned int          nr_entries;
	size_t                total_size;
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
		      struct vmem_pfn_entry __user *entries_ptr,
		      u32 *count);

/* Consumer side */
/* entries_ptr[i].offset must be absolute physical addresses (daemon computes PA). */
struct dma_buf *vmem_create_dmabuf(struct vmem_pfn_entry __user *entries_ptr,
				   u32 count);

/* Source detection */
int vmem_identify_dmabuf_source(int fd,
				u8 bus, u8 dev, u8 fn,
				u8 *src_bus, u8 *src_dev, u8 *src_fn);

/* Lifecycle */
void vmem_import_pins_cleanup(struct vmem_file_priv *priv);
int  vmem_release_pin(struct vmem_file_priv *priv, int orig_fd);

#endif /* _VMEM_DMABUF_H */
