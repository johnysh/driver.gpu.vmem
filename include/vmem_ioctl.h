/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vmem_ioctl.h - vMem driver UAPI (v3.0)
 *
 * Method 1: address translation in KMD via Astera vFE kernel API.
 * Every object is identified by a dma-buf fd; no opaque handles.
 *
 * Origin side:
 * VMEM_IOCTL_OPEN_DMABUF -- attach GPU dma-buf, extract BAR-relative offsets;
 * keeps soft-pin until CLOSE_DMABUF or fd close
 * VMEM_IOCTL_CLOSE_DMABUF -- release soft-pin for one dma-buf (by opened fd)
 *
 * Peer side (Method 1: in-kernel PA synthesis via Astera vFE):
 * VMEM_IOCTL_GET_DMABUF -- input: {node_id, gpu_id, BAR-relative offset list}
 * KMD: astera_lookup(node_id, gpu_id)
 * -> (dbdf, bar, index)
 * -> base = pci_resource_start(dbdf, bar)
 * + index * 32 GB
 * -> PA'[i] = base + offset[i]
 * -> build MMIO dma-buf -> fd
 * VMEM_IOCTL_PUT_DMABUF -- destroy pseudo dma-buf fd
 *
 * Utilities:
 * VMEM_IOCTL_VERSION -- query driver version
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
#define VMEM_DRV_VERSION_MINOR 0
#define VMEM_DRV_VERSION_PATCH 0

#define VMEM_MAX_PFN_ENTRIES 4096

/**
 * struct vmem_pfn_entry - one scatter chunk
 * @offset: OPEN_DMABUF output: BAR2-relative byte offset from GPU VRAM base
 * GET_DMABUF input: same BAR-relative offset (KMD computes PA)
 * @size: page-aligned byte length
 */
struct vmem_pfn_entry {
 __u64 offset;
 __u32 size;
 __u32 pad;
};

/* ── Argument structures ─────────────────────────────────── */

/** struct vmem_ioctl_version_arg - VMEM_IOCTL_VERSION */
struct vmem_ioctl_version_arg {
 __u16 major;
 __u16 minor;
 __u16 patch;
 __u16 reserved;
};

/**
 * struct vmem_ioctl_open_dmabuf_arg - VMEM_IOCTL_OPEN_DMABUF
 *
 * Origin: attach to a GPU dma-buf and extract BAR2-relative scatter offsets.
 * The KMD keeps the attachment alive (soft-pin) so XE TTM cannot evict
 * the VRAM BO while the peer holds the offset list.
 * Release via VMEM_IOCTL_CLOSE_DMABUF or vmem fd close.
 *
 * Two-step count negotiation:
 * Pass count=0 -> KMD returns -ENOSPC with count=required.
 * Allocate entries_ptr[count], call again -> KMD fills the array.
 *
 * @fd: in: GPU dma-buf fd (from zeMemGetIpcHandle)
 * @bus/device/function: in: GPU PCIe BDF
 * @count: in/out: entry buffer capacity / actual entry count
 * @entries_ptr: in: userspace ptr -> vmem_pfn_entry[] output buffer
 */
struct vmem_ioctl_open_dmabuf_arg {
 __s32 fd;
 __u8 bus;
 __u8 device;
 __u8 function;
 __u8 pad2;
 __u32 count;
 __u32 pad;
 __u64 entries_ptr;
};

/**
 * struct vmem_ioctl_get_dmabuf_arg - VMEM_IOCTL_GET_DMABUF (Method 1)
 *
 * Peer: build a pseudo dma-buf from BAR-relative offsets received from origin.
 * KMD calls astera_lookup(node_id, gpu_id) -> (dbdf, bar, index), then:
 * base = pci_resource_start(dbdf, bar) + index * 32 GB
 * PA'[i] = base + entries[i].offset
 *
 * @node_id: in: remote node identifier (forwarded from origin over ctrl channel)
 * @gpu_id: in: remote GPU identifier on that node
 * @count: in: number of BAR-relative offset entries
 * @entries_ptr: in: userspace ptr -> vmem_pfn_entry[] (BAR-relative offsets from origin)
 * @fd: out: pseudo dma-buf fd (import into xe via zeMemOpenIpcHandle)
 */
struct vmem_ioctl_get_dmabuf_arg {
 __u32 node_id;
 __u32 gpu_id;
 __u32 count;
 __u32 pad;
 __u64 entries_ptr;
 __s32 fd;
 __u32 pad2;
};

/**
 * struct vmem_ioctl_put_dmabuf_arg - VMEM_IOCTL_PUT_DMABUF
 * Peer: destroy pseudo dma-buf fd returned by GET_DMABUF.
 * @fd: the fd returned by VMEM_IOCTL_GET_DMABUF
 */
struct vmem_ioctl_put_dmabuf_arg {
 __s32 fd;
 __u32 pad;
};

/**
 * struct vmem_ioctl_close_dmabuf_arg - VMEM_IOCTL_CLOSE_DMABUF
 * Origin: release persistent soft-pin kept since OPEN_DMABUF.
 * @fd: the GPU dma-buf fd originally passed to VMEM_IOCTL_OPEN_DMABUF
 */
struct vmem_ioctl_close_dmabuf_arg {
 __s32 fd;
 __u32 pad;
};

/* ── IOCTL commands ──────────────────────────────────────── */

#define VMEM_IOCTL_VERSION \
 _IOR(VMEM_MAGIC, 0x00, struct vmem_ioctl_version_arg)

/* Origin: attach GPU dma-buf -> BAR-relative scatter offsets + soft-pin */
#define VMEM_IOCTL_OPEN_DMABUF \
 _IOWR(VMEM_MAGIC, 0x01, struct vmem_ioctl_open_dmabuf_arg)

/* Peer: Method 1 - astera_lookup in KMD, PA synthesis, pseudo dma-buf fd */
#define VMEM_IOCTL_GET_DMABUF \
 _IOWR(VMEM_MAGIC, 0x02, struct vmem_ioctl_get_dmabuf_arg)

/* Peer: destroy pseudo dma-buf fd */
#define VMEM_IOCTL_PUT_DMABUF \
 _IOW(VMEM_MAGIC, 0x03, struct vmem_ioctl_put_dmabuf_arg)

/* Origin: release soft-pin (by the opened GPU dma-buf fd) */
#define VMEM_IOCTL_CLOSE_DMABUF \
 _IOW(VMEM_MAGIC, 0x04, struct vmem_ioctl_close_dmabuf_arg)

#endif /* _UAPI_VMEM_IOCTL_H */
