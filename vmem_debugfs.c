// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_debugfs.c - live dma-buf inventory: imported (origin) + exported (peer)
 *
 * /sys/kernel/debug/vmemIntel/imported  -- OPEN_DMABUF soft-pins (origin side)
 *   KMD imported (attached to) the xe dma-buf received from daemon Layer A.
 *   Recorded at OPEN_DMABUF call time.
 *   pid  = opener's process pid
 *   fd   = input xe dma-buf fd
 *   segs = absolute (base, len) per sg element
 *
 * /sys/kernel/debug/vmemIntel/exported  -- GET_DMABUF pseudo dma-bufs (peer side)
 *   KMD exported a pseudo MMIO dma-buf to daemon Layer A.
 *   Recorded at GET_DMABUF call time.
 *   pid  = caller's process pid
 *   fd   = exported pseudo dma-buf fd
 *   segs = mapped TARGET PA (base, len) per chunk
 *
 * Format:
 *   <pid> : fd=<fd> : size=0x<total>
 *          0x<base_10digits> 0x<len>
 *          ...
 *
 * Example:
 *   # /sys/kernel/debug/vmemIntel/imported
 *   4123 : fd=7 : size=0x40000000
 *          0x00a0000000 0x20000000
 *          0x00c0000000 0x20000000
 *
 *   # /sys/kernel/debug/vmemIntel/exported
 *   4130 : fd=11 : size=0x40000000
 *          0x01a0000000 0x20000000
 *          0x01c0000000 0x20000000
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/slab.h>
#include "vmem_buffer.h"
#include "vmem_debugfs.h"

struct vmem_dbg_rec {
	struct list_head link;
	pid_t            pid;
	int              fd;
	size_t           total_size;
	unsigned int     nr_segs;
	struct vmem_seg *segs;
};

/*
 * g_imported: origin side -- KMD attached to xe dma-buf (OPEN_DMABUF)
 * g_exported: peer side   -- KMD exported pseudo dma-buf (GET_DMABUF)
 */
static LIST_HEAD(g_imported);
static LIST_HEAD(g_exported);
static DEFINE_MUTEX(g_lock);
static struct dentry *g_root;

static void dbg_add(struct list_head *list, pid_t pid, int fd, size_t total,
		    const struct vmem_seg *segs, unsigned int nr_segs)
{
	struct vmem_dbg_rec *rec = kzalloc(sizeof(*rec), GFP_KERNEL);
	if (!rec)
		return;
	rec->segs = kvcalloc(nr_segs, sizeof(*segs), GFP_KERNEL);
	if (!rec->segs) {
		kfree(rec);
		return;
	}
	rec->pid        = pid;
	rec->fd         = fd;
	rec->total_size = total;
	rec->nr_segs    = nr_segs;
	memcpy(rec->segs, segs, nr_segs * sizeof(*segs));
	mutex_lock(&g_lock);
	list_add_tail(&rec->link, list);
	mutex_unlock(&g_lock);
}

static void dbg_del(struct list_head *list, pid_t pid, int fd)
{
	struct vmem_dbg_rec *rec, *tmp;

	mutex_lock(&g_lock);
	list_for_each_entry_safe(rec, tmp, list, link) {
		if (rec->pid == pid && rec->fd == fd) {
			list_del(&rec->link);
			kvfree(rec->segs);
			kfree(rec);
			break;
		}
	}
	mutex_unlock(&g_lock);
}

static int vmem_dbg_show(struct seq_file *m, void *v)
{
	struct list_head *list = m->private;
	struct vmem_dbg_rec *rec;
	unsigned int i;

	mutex_lock(&g_lock);
	list_for_each_entry(rec, list, link) {
		seq_printf(m, "%d : fd=%d : size=0x%zx\n",
			   rec->pid, rec->fd, rec->total_size);
		for (i = 0; i < rec->nr_segs; i++)
			seq_printf(m, "       0x%010llx 0x%zx\n",
				   (unsigned long long)rec->segs[i].base,
				   rec->segs[i].len);
	}
	mutex_unlock(&g_lock);
	return 0;
}

static int dbg_open_imported(struct inode *inode, struct file *file)
{
	return single_open(file, vmem_dbg_show, &g_imported);
}

static int dbg_open_exported(struct inode *inode, struct file *file)
{
	return single_open(file, vmem_dbg_show, &g_exported);
}

static const struct file_operations fops_imported = {
	.owner   = THIS_MODULE,
	.open    = dbg_open_imported,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static const struct file_operations fops_exported = {
	.owner   = THIS_MODULE,
	.open    = dbg_open_exported,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

int vmem_debugfs_init(void)
{
	g_root = debugfs_create_dir("vmemIntel", NULL);
	if (IS_ERR_OR_NULL(g_root)) {
		pr_warn("vmem: debugfs unavailable\n");
		g_root = NULL;
		return 0;
	}
	debugfs_create_file("imported", 0444, g_root, NULL, &fops_imported);
	debugfs_create_file("exported", 0444, g_root, NULL, &fops_exported);
	pr_info("vmem: debugfs at /sys/kernel/debug/vmemIntel/{imported,exported}\n");
	return 0;
}

void vmem_debugfs_exit(void)
{
	debugfs_remove_recursive(g_root);
	g_root = NULL;
}

/* ── Origin side (OPEN_DMABUF → /imported): KMD attached to xe dma-buf ─── */

void vmem_debugfs_add_imported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs)
{
	dbg_add(&g_imported, pid, fd, total, segs, nr_segs);
}

void vmem_debugfs_del_imported(pid_t pid, int fd)
{
	dbg_del(&g_imported, pid, fd);
}

/* ── Peer side (GET_DMABUF → /exported): KMD exported pseudo dma-buf ────── */

void vmem_debugfs_add_exported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs)
{
	dbg_add(&g_exported, pid, fd, total, segs, nr_segs);
}

void vmem_debugfs_del_exported(pid_t pid, int fd)
{
	dbg_del(&g_exported, pid, fd);
}
