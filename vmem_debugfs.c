// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_debugfs.c - global imported/exported registry + debugfs rendering
 *
 * Two lists (g_imported / g_exported) under one mutex.
 * Entries appear on OPEN_DMABUF/GET_DMABUF, disappear on CLOSE_DMABUF/PUT_DMABUF
 * or the fd-close backstop.  Rendered via seq_file.
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

static LIST_HEAD(g_imported);
static LIST_HEAD(g_exported);
static DEFINE_MUTEX(g_lock);
static struct dentry *g_root;

static void dbg_add(struct list_head *list, pid_t pid, int fd, size_t total,
		    const struct vmem_seg *segs, unsigned int nr_segs)
{
	struct vmem_dbg_rec *rec = kzalloc(sizeof(*rec), GFP_KERNEL);
	if (!rec) return;
	rec->segs = kvcalloc(nr_segs, sizeof(*segs), GFP_KERNEL);
	if (!rec->segs) { kfree(rec); return; }
	rec->pid = pid; rec->fd = fd;
	rec->total_size = total; rec->nr_segs = nr_segs;
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
			seq_printf(m, "0x%llx 0x%zx\n",
				   (unsigned long long)rec->segs[i].base,
				   rec->segs[i].len);
	}
	mutex_unlock(&g_lock);
	return 0;
}

static int dbg_open_imported(struct inode *inode, struct file *file)
{ return single_open(file, vmem_dbg_show, &g_imported); }

static int dbg_open_exported(struct inode *inode, struct file *file)
{ return single_open(file, vmem_dbg_show, &g_exported); }

static const struct file_operations fops_imported = {
	.owner = THIS_MODULE, .open = dbg_open_imported,
	.read = seq_read, .llseek = seq_lseek, .release = single_release,
};
static const struct file_operations fops_exported = {
	.owner = THIS_MODULE, .open = dbg_open_exported,
	.read = seq_read, .llseek = seq_lseek, .release = single_release,
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
	pr_info("vmem: debugfs at /sys/kernel/debug/vmemIntel/\n");
	return 0;
}

void vmem_debugfs_exit(void)
{
	debugfs_remove_recursive(g_root);
	g_root = NULL;
}

void vmem_debugfs_add_imported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs)
{ dbg_add(&g_imported, pid, fd, total, segs, nr_segs); }

void vmem_debugfs_del_imported(pid_t pid, int fd)
{ dbg_del(&g_imported, pid, fd); }

void vmem_debugfs_add_exported(pid_t pid, int fd, size_t total,
			       const struct vmem_seg *segs, unsigned int nr_segs)
{ dbg_add(&g_exported, pid, fd, total, segs, nr_segs); }

void vmem_debugfs_del_exported(pid_t pid, int fd)
{ dbg_del(&g_exported, pid, fd); }
