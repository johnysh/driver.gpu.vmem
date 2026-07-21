/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vmem_ioctl.h - vMem driver UAPI (v4.0)
 *
 * Method 2: pass-through only. KMD and UMD do NOT perform address
 * translation. The IMEMLINK daemon (Layer A) calls the Astera COSMOS SDK
 * in userspace to translate origin GPU BAR-relative offsets to peer-side
 * absolute physical addresses (TARGET PAs). The translated PAs are passed
 * down to the peer UMD, which forwards them verbatim to KMD.
 *
 * Origin side:
 *   VMEM_IOCTL_OPEN_DMABUF   -- KMD attaches GPU dma-buf, walks sg_table,
 *                               returns ABSOLUTE physical addresses (abs PAs)
 *                               per scatter chunk. UMD forwards these abs PAs
 *                               to the IMEMLINK daemon as-is (no BAR subtraction).
 *   VMEM_IOCTL_CLOSE_DMABUF  -- release soft-pin (keyed by opened dma-buf fd)
 *
 * Peer side (Method 2: pass-through, daemon does translation via COSMOS SDK):
 *   VMEM_IOCTL_GET_DMABUF    -- input: {count, abs_pa entries[]}
 *                               KMD: build MMIO dma-buf from pre-translated abs PAs.
 *                               No astera_lookup in KMD. Translation was done by
 *                               daemon Layer A via cosmos_query_vfe().
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

#define VMEM_DRV_VERSION_MAJOR 4
#define VMEM_DRV_VERSION_MINOR 0
#define VMEM_DRV_VERSION_PATCH 0

#define VMEM_MAX_PFN_ENTRIES 4096

/**
 * struct vmem_pfn_entry - one scatter chunk descriptor
 *
 * OPEN_DMABUF output (origin side, filled by KMD):
 *   @addr = absolute physical address of the scatter chunk
 *           (sg_dma_address(sg), which equals PA when IOMMU is off).
 *           UMD forwards this value to the daemon without modification.
 *
 * GET_DMABUF input (peer side, filled by UMD with daemon-provided value):
 *   @addr = absolute peer-side physical address (TARGET PA) already
 *           translated by daemon Layer A via Astera COSMOS SDK:
 *             cosmos_query_vfe(node_id, gpu_id) -> {vfe_dbdf, bar_id, slot}
 *             PA'[i] = pci_resource_start(vfe_dbdf, bar_id)
 *                      + slot * 32 GiB + bar_offset[i]
 *           KMD uses this value directly without further translation.
 *
 * @addr:  absolute physical address (abs PA origin, or translated PA' on peer)
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
 *   Forward entries[i].addr (abs PAs) to the IMEMLINK daemon verbatim.
 *   The daemon (Layer A) handles any BAR-relative normalization and stores
 *   the descriptor in its BlobStore. No offset subtraction in UMD.
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
 * struct vmem_ioctl_get_dmabuf_arg - VMEM_IOCTL_GET_DMABUF (Method 2)
 *
 * Peer: UMD fills entries[i].addr with absolute peer-side physical addresses
 * (TARGET PAs) already translated by the IMEMLINK daemon via Astera COSMOS SDK.
 * KMD builds the MMIO pseudo dma-buf directly from these PAs. No astera_lookup
 * in KMD.
 *
 * @count:       in:  number of entries
 * @entries_ptr: in:  userspace ptr -> vmem_pfn_entry[] (addr = translated abs PA)
 * @fd:          out: pseudo dma-buf fd
 */
struct vmem_ioctl_get_dmabuf_arg {
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
