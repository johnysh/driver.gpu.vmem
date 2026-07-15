/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VMEM_BUFFER_H
#define _VMEM_BUFFER_H

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/sched.h>

struct vmem_seg {
	phys_addr_t base;
	size_t      len;
};

struct vmem_buffer_obj {
	struct kref        kref;
	struct list_head   link;
	int                dmabuf_fd;
	pid_t              pid;
	size_t             total_size;
	unsigned int       nr_segs;
	struct vmem_seg   *segs;
};

struct vmem_buffer_obj *vmem_buffer_obj_alloc(unsigned int nr_segs, size_t total_size);
void vmem_buffer_obj_get(struct vmem_buffer_obj *obj);
void vmem_buffer_obj_put(struct vmem_buffer_obj *obj);

#endif
