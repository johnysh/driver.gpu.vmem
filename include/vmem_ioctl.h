/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vmem_ioctl.h - vMem driver UAPI (v2.1)
 *
 * Consumer (peer) side only — origin-side scatter extraction removed.
 *
 * The KMD imports the Astera vFE driver and calls its in-kernel lookup:
 *   astera_lookup(node_id, gpu_id) -> (dbdf, bar, index)
 *   base = pci_resource_start(dbdf, bar) + index * 32 GB
 *   PA'  = base + offset
 *   -> build MMIO dma-buf
 *
 * VMEM_IOCTL_GET_IPC_HANDLE  -- build pseudo dma-buf from {node_id, gpu_id, offsets}
 * VMEM_IOCTL_PUT_IPC_HANDLE  -- destroy pseudo dma-buf fd
 * VMEM_IOCTL_VERSION         -- query driver version
 * VMEM_IOCTL_IDENTIFY_DMABUF -- probe which GPU owns a dma-buf
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
#define VMEM_DRV_VERSION_MINOR 1
#define VMEM_DRV_VERSION_PATCH 0

/* Recommended userspace buffer size for vmem_pfn_entry[] (not a hard kernel limit) */
#define VMEM_MAX_PFN_ENTRIES 4096

/**
 * struct vmem_pfn_entry - one scatter chunk
 * @offset: BAR-relative byte offset from the origin GPU's VRAM base
 *          (produced by the origin node; KMD adds vFE window base to get PA')
 * @size:   byte length of this chunk (page-aligned)
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
 * struct vmem_ioctl_get_ipc_handle_arg - VMEM_IOCTL_GET_IPC_HANDLE
 *
 * Input: BAR-relative scatter offsets produced by the origin node, forwarded
 * over the control channel together with the origin's node_id and gpu_id.
 *
 * KMD steps:
 *   1. astera_lookup(node_id, gpu_id) -> (dbdf, bar, index)
 *   2. base = pci_resource_start(dbdf, bar) + index * 32 GB
 *   3. PA'[i] = base + entries[i].offset
 *   4. build MMIO pseudo dma-buf backed by PA' array
 *
 * @node_id:     in:  remote node identifier
 * @gpu_id:      in:  remote GPU identifier on that node
 * @count:       in:  number of entries in entries_ptr[]
 * @entries_ptr: in:  userspace ptr -> vmem_pfn_entry[] (BAR-relative offsets)
 * @fd:          out: pseudo dma-buf fd (import into xe via zeMemOpenIpcHandle)
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
 * Destroy the pseudo dma-buf fd returned by GET_IPC_HANDLE.
 * Equivalent to close(fd); provided for lifecycle symmetry.
 */
struct vmem_ioctl_put_ipc_handle_arg {
	__s32 fd;
	__u32 pad;
};

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

/* 0x01: reserved (was OPEN_IPC_HANDLE / GET_PFN_OFFSET_LIST) */

/* Build pseudo dma-buf via in-kernel Astera vFE lookup */
#define VMEM_IOCTL_GET_IPC_HANDLE \
	_IOWR(VMEM_MAGIC, 0x02, struct vmem_ioctl_get_ipc_handle_arg)

/* Destroy pseudo dma-buf fd */
#define VMEM_IOCTL_PUT_IPC_HANDLE \
	_IOW(VMEM_MAGIC, 0x03, struct vmem_ioctl_put_ipc_handle_arg)

/* 0x04: reserved (was CLOSE_IPC_HANDLE / PUT_IPC_FD) */

/* 0x05: reserved */

#define VMEM_IOCTL_IDENTIFY_DMABUF \
	_IOWR(VMEM_MAGIC, 0x06, struct vmem_ioctl_identify_dmabuf_arg)

#endif /* _UAPI_VMEM_IOCTL_H */
