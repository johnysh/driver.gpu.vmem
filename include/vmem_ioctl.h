/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vmem_ioctl.h - vMem driver UAPI (v2.0)
 *
 * Origin (local GPU owner):
 *   VMEM_IOCTL_OPEN_IPC_HANDLE   -- attach GPU dma-buf, extract BAR-relative offsets
 *   VMEM_IOCTL_CLOSE_IPC_HANDLE  -- release persistent soft-pin for one dma-buf
 *
 * Peer (remote GPU importer) - Method 1: in-kernel PA synthesis via Astera vFE:
 *   VMEM_IOCTL_GET_IPC_HANDLE    -- build pseudo dma-buf from {node_id, gpu_id, offsets}
 *                                   KMD calls astera_lookup -> computes PA internally
 *   VMEM_IOCTL_PUT_IPC_HANDLE    -- destroy pseudo dma-buf, release peer resources
 *
 * Utilities:
 *   VMEM_IOCTL_VERSION           -- query driver version
 *   VMEM_IOCTL_IDENTIFY_DMABUF   -- probe which GPU owns a dma-buf
 *
 * v2.0 changes from v1.0:
 *   - GET_IPC_FD replaced by GET_IPC_HANDLE:
 *       old: daemon passes absolute PAs (vFE_bar_base + offset) to KMD
 *       new: KMD calls astera_lookup(node_id, gpu_id) -> PA synthesis in kernel
 *   - Renamed ioctls to match IPC lifecycle: open/get/put/close
 *   - Backward compat aliases kept for v1.0 names (same ioctl numbers)
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

#define VMEM_DRV_VERSION_MAJOR 2
#define VMEM_DRV_VERSION_MINOR 0
#define VMEM_DRV_VERSION_PATCH 0

/* Recommended userspace buffer size for vmem_pfn_entry[] (not a hard kernel limit) */
#define VMEM_MAX_PFN_ENTRIES 4096

/**
 * struct vmem_pfn_entry - one scatter chunk for cross-node PFN transport
 * @offset: origin side:  BAR2-relative byte offset from GPU VRAM base
 *          peer side:    BAR-relative offset passed to GET_IPC_HANDLE
 *                        (KMD translates to absolute PA internally)
 * @size:   byte length of this scatter chunk (page-aligned)
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
 * struct vmem_ioctl_open_ipc_handle_arg - VMEM_IOCTL_OPEN_IPC_HANDLE
 *
 * Origin: attach to GPU dma-buf, extract BAR2-relative scatter offsets.
 * Driver keeps attachment alive (soft-pin) until CLOSE_IPC_HANDLE or fd close.
 *
 * @fd:          in:  GPU dma-buf fd (from zeMemGetIpcHandle)
 * @bus/device/function: in: GPU PCIe BDF
 * @count:       in/out: entry buffer capacity / actual entry count
 *               Returns -ENOSPC with count=required if buffer too small -> retry
 * @entries_ptr: in:  userspace ptr -> vmem_pfn_entry[] output buffer
 *               out: entries[i].offset = BAR2-relative byte offset
 */
struct vmem_ioctl_open_ipc_handle_arg {
	__s32  fd;
	__u8   bus;
	__u8   device;
	__u8   function;
	__u8   pad2;
	__u32  count;
	__u32  pad;
	__u64  entries_ptr;
};

/* Backward compat alias (v1.0 name, identical struct) */
#define vmem_ioctl_get_pfn_arg  vmem_ioctl_open_ipc_handle_arg

/**
 * struct vmem_ioctl_get_ipc_handle_arg - VMEM_IOCTL_GET_IPC_HANDLE (Method 1)
 *
 * Peer: build a pseudo dma-buf from BAR-relative offsets received from origin.
 * The KMD calls astera_lookup(node_id, gpu_id) to obtain the vFE endpoint, then:
 *   base = pci_resource_start(vfe_pdev, ep.bar) + ep.index * 32 GiB
 *   PA'  = base + entries[i].offset
 * and wraps the resulting MMIO addresses into a synthetic dma-buf.
 *
 * @node_id:     in:  remote node identifier (sent over control path by origin)
 * @gpu_id:      in:  remote GPU identifier on that node
 * @count:       in:  number of entries in entries_ptr[]
 * @entries_ptr: in:  userspace ptr -> vmem_pfn_entry[] (BAR-relative offsets)
 * @fd:          out: pseudo dma-buf fd for zeMemOpenIpcHandle
 */
struct vmem_ioctl_get_ipc_handle_arg {
	__u32  node_id;
	__u32  gpu_id;
	__u32  count;
	__u32  pad;
	__u64  entries_ptr;
	__s32  fd;
	__u32  pad2;
};

/**
 * struct vmem_ioctl_put_ipc_handle_arg - VMEM_IOCTL_PUT_IPC_HANDLE
 *
 * Peer: destroy the pseudo dma-buf fd created by GET_IPC_HANDLE.
 * Equivalent to close(fd) from userspace; provided for lifecycle symmetry.
 */
struct vmem_ioctl_put_ipc_handle_arg {
	__s32 fd;
	__u32 pad;
};

/* Backward compat alias (v1.0 CLOSE_IPC_FD, identical struct) */
#define vmem_ioctl_close_ipc_fd_arg  vmem_ioctl_put_ipc_handle_arg

/**
 * struct vmem_ioctl_close_ipc_handle_arg - VMEM_IOCTL_CLOSE_IPC_HANDLE
 *
 * Origin: release persistent soft-pin kept since OPEN_IPC_HANDLE.
 * Triggers: dma_buf_unmap -> dma_buf_detach -> dma_buf_put.
 * @fd: original GPU dma-buf fd passed to OPEN_IPC_HANDLE
 */
struct vmem_ioctl_close_ipc_handle_arg {
	__s32 fd;
	__u32 pad;
};

/* Backward compat alias (v1.0 PUT_IPC_FD, identical struct) */
#define vmem_ioctl_put_ipc_fd_arg  vmem_ioctl_close_ipc_handle_arg

/**
 * struct vmem_ioctl_identify_dmabuf_arg - VMEM_IOCTL_IDENTIFY_DMABUF
 *
 * Probe which GPU's VRAM backs a dma-buf via temporary attach + BAR scan.
 */
struct vmem_ioctl_identify_dmabuf_arg {
	__s32  fd;
	__u8   bus;
	__u8   device;
	__u8   function;
	__u8   pad;
	__u8   source_bus;
	__u8   source_device;
	__u8   source_function;
	__u8   pad2;
};

/* ── IOCTL commands ──────────────────────────────────────── */

#define VMEM_IOCTL_VERSION \
	_IOR(VMEM_MAGIC, 0x00, struct vmem_ioctl_version_arg)

/* Origin: export GPU dma-buf -> BAR-relative scatter offsets */
#define VMEM_IOCTL_OPEN_IPC_HANDLE \
	_IOWR(VMEM_MAGIC, 0x01, struct vmem_ioctl_open_ipc_handle_arg)

/* Peer: Method 1 - KMD synthesizes PA via astera_lookup, builds pseudo dma-buf */
#define VMEM_IOCTL_GET_IPC_HANDLE \
	_IOWR(VMEM_MAGIC, 0x02, struct vmem_ioctl_get_ipc_handle_arg)

/* Peer: destroy pseudo dma-buf fd */
#define VMEM_IOCTL_PUT_IPC_HANDLE \
	_IOW(VMEM_MAGIC, 0x03, struct vmem_ioctl_put_ipc_handle_arg)

/* Origin: release soft-pin for a GPU dma-buf */
#define VMEM_IOCTL_CLOSE_IPC_HANDLE \
	_IOW(VMEM_MAGIC, 0x04, struct vmem_ioctl_close_ipc_handle_arg)

/* 0x05: reserved */

#define VMEM_IOCTL_IDENTIFY_DMABUF \
	_IOWR(VMEM_MAGIC, 0x06, struct vmem_ioctl_identify_dmabuf_arg)

/* ── Backward compatibility aliases (v1.0 names) ────────── */
#define VMEM_IOCTL_GET_PFN_OFFSET_LIST  VMEM_IOCTL_OPEN_IPC_HANDLE
#define VMEM_IOCTL_PUT_IPC_FD           VMEM_IOCTL_CLOSE_IPC_HANDLE
#define VMEM_IOCTL_CLOSE_IPC_FD         VMEM_IOCTL_PUT_IPC_HANDLE

#endif /* _UAPI_VMEM_IOCTL_H */
