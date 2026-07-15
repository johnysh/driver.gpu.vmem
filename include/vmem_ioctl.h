/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vmem_ioctl.h - vMem driver UAPI (v3.1)
 *
 * Method 1: address translation in KMD via Astera vFE kernel API.
 * Every object is identified by a dma-buf fd; no opaque handles.
 *
 * Origin side:
 *   VMEM_IOCTL_OPEN_DMABUF   -- KMD attaches GPU dma-buf, walks sg_table,
 *                               returns ABSOLUTE physical addresses (abs PAs)
 *                               per scatter chunk.  UMD then computes:
 *                                 offset[i] = entries[i].addr - gpu_bar2_base
 *                               and forwards {offset list, node_id, gpu_id}
 *                               to the peer over the control channel.
 *   VMEM_IOCTL_CLOSE_DMABUF  -- release soft-pin (keyed by opened dma-buf fd)
 *
 * Peer side (Method 1: in-kernel PA synthesis via Astera vFE):
 *   VMEM_IOCTL_GET_DMABUF    -- input: {node_id, gpu_id, BAR-relative offset list}
 *                               KMD: astera_lookup(node_id, gpu_id)
 *                                    -> (dbdf, bar, index)
 *                                    -> base = pci_resource_start(dbdf, bar)
 *                                           + index * 32 GB
 *                                    -> PA'[i] = base + entries[i].addr
 *                                    -> build MMIO dma-buf -> fd
 *   VMEM_IOCTL_PUT_DMABUF    -- destroy pseudo dma-buf fd
 *
 * Utilities:
 *   VMEM_IOCTL_VERSION        -- query driver version
 */
#ifndef _UAPI_VMEM_IOCTL_H
#define _UAPI_VMEM_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#include <asm/types.h>
#endif

#define VMEM_MAGIC 'V'

#define VMEM_DRV_VERSION_MAJOR 3
#define VMEM_DRV_VERSION_MINOR 1
#define VMEM_DRV_VERSION_PATCH 0

#define VMEM_MAX_PFN_ENTRIES 4096

/**
 * struct vmem_pfn_entry - one scatter chunk descriptor
 *
 * Dual-use: the meaning of @addr differs by ioctl context.
 *
 * OPEN_DMABUF output (origin side, filled by KMD):
 *   @addr = absolute physical address of the scatter chunk
 *           (sg_dma_address(sg), which equals PA when IOMMU is off)
 *   UMD computes: offset[i] = addr[i] - gpu_bar2_base
 *
 * GET_DMABUF input (peer side, filled by UMD):
 *   @addr = BAR-relative byte offset forwarded from origin
 *           (offset[i] = abs_pa[i] - gpu_bar2_base)
 *   KMD computes: PA'[i] = vfe_base + addr[i]
 *
 * @addr:  absolute physical address (OPEN) or BAR-relative offset (GET)
 * @size:  page-aligned byte length of this chunk
 */
struct vmem_pfn_entry {
	__u64 addr;
	__u32 size;
	__u32 pad;
};

/* ── Argument structures ─────────────────────────────────── */

struct vmem_ioctl_version_arg {
	__u16 major;
	__u16 minor;
	__u16 patch;
	__u16 reserved;
};

/**
 * struct vmem_ioctl_open_dmabuf_arg - VMEM_IOCTL_OPEN_DMABUF
 *
 * Origin: KMD attaches to the GPU dma-buf (P2P dynamic attach), walks the
 * sg_table, and returns one vmem_pfn_entry per scatter chunk with the
 * ABSOLUTE physical address in entries[i].addr.
 *
 * UMD responsibility after this call:
 *   bar2_base = read_sysfs_bar2(bus, device, function);
 *   for i in 0..count:
 *       offset[i] = entries[i].addr - bar2_base
 *   send {offset list, node_id, gpu_id} to peer over control channel.
 *
 * Two-step count negotiation:
 *   Pass count=0 -> KMD returns -ENOSPC with count=required N.
 *   Allocate entries_ptr[N], call again -> KMD fills entries[].
 *
 * @fd:               in:  GPU dma-buf fd (xe-exported)
 * @bus/device/function: in: GPU PCIe BDF (used for P2P attach)
 * @count:            in/out: capacity / actual chunk count (-ENOSPC retry)
 * @page_size:        out: PAGE_SIZE (4096)
 * @total_size:       out: total bytes across all chunks
 * @entries_ptr:      in:  userspace ptr -> vmem_pfn_entry[] to fill
 */
struct vmem_ioctl_open_dmabuf_arg {
	__s32  fd;
	__u8   bus;
	__u8   device;
	__u8   function;
	__u8   pad2;
	__u32  count;
	__u32  page_size;    /* out: PAGE_SIZE */
	__u64  total_size;   /* out: total bytes */
	__u64  entries_ptr;  /* in:  userspace ptr -> vmem_pfn_entry[] output */
};

/**
 * struct vmem_ioctl_get_dmabuf_arg - VMEM_IOCTL_GET_DMABUF (Method 1)
 *
 * Peer: UMD fills entries[i].addr with BAR-relative offsets received from
 * origin over the control channel.  KMD resolves the vFE endpoint and
 * synthesises absolute peer-side PAs:
 *   astera_lookup(node_id, gpu_id) -> (dbdf, bar, index)
 *   vfe_base  = pci_resource_start(dbdf, bar) + index * 32 GB
 *   PA'[i]   = vfe_base + entries[i].addr
 *
 * @node_id:     in:  remote node id (forwarded from origin)
 * @gpu_id:      in:  remote GPU id
 * @count:       in:  number of BAR-relative offset entries
 * @entries_ptr: in:  userspace ptr -> vmem_pfn_entry[] (addr = BAR-relative)
 * @fd:          out: pseudo dma-buf fd
 */
struct vmem_ioctl_get_dmabuf_arg {
	__u32  node_id;
	__u32  gpu_id;
	__u32  count;
	__u32  pad;
	__u64  entries_ptr;
	__s32  fd;
	__u32  pad2;
};

struct vmem_ioctl_put_dmabuf_arg {
	__s32 fd;
	__u32 pad;
};

struct vmem_ioctl_close_dmabuf_arg {
	__s32 fd;
	__u32 pad;
};

/* ── IOCTL commands ──────────────────────────────────────── */

#define VMEM_IOCTL_VERSION \
	_IOR(VMEM_MAGIC, 0x00, struct vmem_ioctl_version_arg)

#define VMEM_IOCTL_OPEN_DMABUF \
	_IOWR(VMEM_MAGIC, 0x01, struct vmem_ioctl_open_dmabuf_arg)

#define VMEM_IOCTL_GET_DMABUF \
	_IOWR(VMEM_MAGIC, 0x02, struct vmem_ioctl_get_dmabuf_arg)

#define VMEM_IOCTL_PUT_DMABUF \
	_IOW(VMEM_MAGIC, 0x03, struct vmem_ioctl_put_dmabuf_arg)

#define VMEM_IOCTL_CLOSE_DMABUF \
	_IOW(VMEM_MAGIC, 0x04, struct vmem_ioctl_close_dmabuf_arg)

#endif /* _UAPI_VMEM_IOCTL_H */
