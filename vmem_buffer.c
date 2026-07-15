// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/kref.h>
#include "vmem_buffer.h"

static void vmem_buffer_obj_release(struct kref *kref)
{
	struct vmem_buffer_obj *obj =
		container_of(kref, struct vmem_buffer_obj, kref);
	pr_debug("vmem: buffer_obj release fd=%d\n", obj->dmabuf_fd);
	kvfree(obj->segs);
	kfree(obj);
}

struct vmem_buffer_obj *vmem_buffer_obj_alloc(unsigned int nr_segs, size_t total_size)
{
	struct vmem_buffer_obj *obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return NULL;
	obj->segs = kvcalloc(nr_segs, sizeof(*obj->segs), GFP_KERNEL);
	if (!obj->segs) { kfree(obj); return NULL; }
	kref_init(&obj->kref);
	INIT_LIST_HEAD(&obj->link);
	obj->nr_segs    = nr_segs;
	obj->total_size = total_size;
	return obj;
}

void vmem_buffer_obj_get(struct vmem_buffer_obj *obj) { kref_get(&obj->kref); }
void vmem_buffer_obj_put(struct vmem_buffer_obj *obj)
{
	kref_put(&obj->kref, vmem_buffer_obj_release);
}
