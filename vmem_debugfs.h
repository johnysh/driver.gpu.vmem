/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmem_debugfs.h - live dma-buf inventory via debugfs
 *
 * /sys/kernel/debug/vmemIntel/imported  -- OPEN_DMABUF soft-pins (origin side)
 *   KMD imported (attached to) the xe dma-buf from daemon Layer A.
 *   pid  = opener's process pid
 *   fd   = input xe dma-buf fd
 *   segs = absolute (base, len) per sg element
 *
 * /sys/kernel/debug/vmemIntel/exported  -- GET_DMABUF pseudo dma-bufs (peer side)
 *   KMD exported a pseudo MMIO dma-buf to daemon Layer A.
 *   pid  = caller's process pid
 *   fd   = exported pseudo dma-buf fd
 *   segs = mapped TARGET PA (base, len) per chunk
 *
 * Format:
 *   <pid> : fd=<fd> : size=0x<total>
 *          0x<base_10digits> 0x<len>
 *          ...
 */
#ifndef _VMEM_DEBUGFS_H
#define _VMEM_DEBUGFS_H

#include <linux/types.h>

struct vmem_seg;

int  vmem_debugfs_init(void);
void vmem_debugfs_exit(void);

/* Origin side (OPEN_DMABUF → /imported): KMD attached to xe dma-buf */
void vmem_debugfs_add_imported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs);
void vmem_debugfs_del_imported(pid_t pid, int fd);

/* Peer side (GET_DMABUF → /exported): KMD exported pseudo dma-buf */
void vmem_debugfs_add_exported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs);
void vmem_debugfs_del_exported(pid_t pid, int fd);

#endif /* _VMEM_DEBUGFS_H */
