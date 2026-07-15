/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmem_debugfs.h - read-only debugfs inventory of live dma-buf descriptors
 *
 * /sys/kernel/debug/vmemIntel/imported  -- live OPEN_DMABUF attachments
 * /sys/kernel/debug/vmemIntel/exported  -- live GET_DMABUF pseudo dma-bufs
 *
 * Format per descriptor:
 *   <pid> : fd=<fd> : size=0x<total>
 *   0x<phys_base> 0x<len>
 *   ...
 */
#ifndef _VMEM_DEBUGFS_H
#define _VMEM_DEBUGFS_H

#include <linux/types.h>

struct vmem_seg;

int  vmem_debugfs_init(void);
void vmem_debugfs_exit(void);

void vmem_debugfs_add_imported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs);
void vmem_debugfs_del_imported(pid_t pid, int fd);

void vmem_debugfs_add_exported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs);
void vmem_debugfs_del_exported(pid_t pid, int fd);

#endif /* _VMEM_DEBUGFS_H */
