// SPDX-License-Identifier: GPL-2.0
/*
 * vmem_astera.c - Astera vFE driver registration stub
 *
 * Provides a function-pointer registration interface so that astera_vfe.ko
 * can plug in the real astera_lookup() symbol at runtime without vmem.ko
 * having a hard link-time dependency on astera_vfe.ko.
 *
 * Dependency order:
 *   vmem.ko loads first  -> exports vmem_register/unregister_astera_lookup
 *   astera_vfe.ko loads  -> calls vmem_register_astera_lookup(my_fn)
 *   astera_vfe.ko unloads -> calls vmem_unregister_astera_lookup(my_fn)
 *
 * Without astera_vfe.ko, vmem_astera_lookup() returns -ENOSYS.
 */

#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include "vmem_astera.h"

static astera_lookup_fn_t g_lookup_fn;
static DEFINE_SPINLOCK(g_lookup_lock);

void vmem_register_astera_lookup(astera_lookup_fn_t fn)
{
	spin_lock(&g_lookup_lock);
	g_lookup_fn = fn;
	spin_unlock(&g_lookup_lock);
	pr_info("vmem: Astera vFE lookup registered\n");
}
EXPORT_SYMBOL(vmem_register_astera_lookup);

void vmem_unregister_astera_lookup(astera_lookup_fn_t fn)
{
	spin_lock(&g_lookup_lock);
	if (g_lookup_fn == fn)
		g_lookup_fn = NULL;
	spin_unlock(&g_lookup_lock);
	pr_info("vmem: Astera vFE lookup unregistered\n");
}
EXPORT_SYMBOL(vmem_unregister_astera_lookup);

int vmem_astera_lookup(u32 node_id, u32 gpu_id, struct astera_vfe_ep *ep)
{
	astera_lookup_fn_t fn;
	int ret;

	spin_lock(&g_lookup_lock);
	fn = g_lookup_fn;
	spin_unlock(&g_lookup_lock);

	if (!fn) {
		pr_warn_once("vmem: vmem_astera_lookup: vFE driver not registered\n");
		return -ENOSYS;
	}

	ret = fn(node_id, gpu_id, ep);
	if (ret)
		pr_debug("vmem: astera_lookup(node=%u gpu=%u) -> %d\n",
			 node_id, gpu_id, ret);
	else
		pr_debug("vmem: astera_lookup(node=%u gpu=%u) -> "
			 "%04x:%02x:%02x.%u BAR%u idx=%u\n",
			 node_id, gpu_id,
			 ep->domain, ep->bus,
			 PCI_SLOT(ep->devfn), PCI_FUNC(ep->devfn),
			 ep->bar, ep->index);
	return ret;
}
