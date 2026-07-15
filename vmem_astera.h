/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmem_astera.h - Astera vFE driver interface for vMem KMD
 *
 * The Astera vFE driver (astera_vfe.ko) registers its lookup function via
 * vmem_register_astera_lookup() on init and unregisters on exit.
 *
 * vmem.ko calls vmem_astera_lookup() internally to translate (node_id, gpu_id)
 * into a physical vFE endpoint, from which the peer-side MMIO window base is
 * derived:
 *
 *   base = pci_resource_start(vfe_pdev, ep.bar) + ep.index * VMEM_VFE_WINDOW_SIZE
 *   PA'  = base + scatter_offset          (Method 1: in-kernel PA synthesis)
 *
 * Returns -ENOSYS when no vFE driver is registered (stub/fallback path).
 */
#ifndef _VMEM_ASTERA_H
#define _VMEM_ASTERA_H

#include <linux/types.h>

/**
 * struct astera_vfe_ep - vFE endpoint descriptor returned by astera_lookup
 * @domain: PCI domain
 * @bus:    PCI bus of the vFE device as seen on the consumer host
 * @devfn:  PCI devfn of the vFE device (use PCI_SLOT/PCI_FUNC to unpack)
 * @bar:    BAR index whose window maps the remote GPU VRAM (typically 2)
 * @index:  window slot within that BAR;
 *          base_pa = pci_resource_start(vfe_pdev, bar) + index * VMEM_VFE_WINDOW_SIZE
 */
struct astera_vfe_ep {
	u16 domain;
	u8  bus;
	u8  devfn;
	u8  bar;
	u32 index;
};

/* 32 GiB per vFE window slot (matches Astera PEX890xx fabric architecture) */
#define VMEM_VFE_WINDOW_SIZE  (32ULL << 30)

typedef int (*astera_lookup_fn_t)(u32 node_id, u32 gpu_id,
				  struct astera_vfe_ep *ep);

/**
 * vmem_register_astera_lookup - register Astera vFE lookup callback
 *
 * Called by astera_vfe.ko on module init.
 * Exported by vmem.ko so astera_vfe.ko can depend on it at load time.
 */
void vmem_register_astera_lookup(astera_lookup_fn_t fn);

/**
 * vmem_unregister_astera_lookup - unregister Astera vFE lookup callback
 *
 * Called by astera_vfe.ko on module exit.
 */
void vmem_unregister_astera_lookup(astera_lookup_fn_t fn);

/**
 * vmem_astera_lookup - resolve a remote GPU to its vFE endpoint
 * @node_id: remote node identifier (assigned by system software / daemon)
 * @gpu_id:  remote GPU slot on that node
 * @ep:      output: filled with (domain, bus, devfn, bar, index) on success
 *
 * Return:  0        filled *ep, caller can compute base_pa
 *          -ENOSYS  no vFE driver currently registered
 *          -ENODEV  (node_id, gpu_id) pair not in vFE endpoint table
 */
int vmem_astera_lookup(u32 node_id, u32 gpu_id, struct astera_vfe_ep *ep);

#endif /* _VMEM_ASTERA_H */
