/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vmem_ioctl.h - vMem driver UAPI (v1.0)
 *
 * Producer (local GPU owner):
 *   VMEM_IOCTL_GET_PFN_OFFSET_LIST  -- attach GPU dma-buf, extract BAR offsets
 *   VMEM_IOCTL_PUT_IPC_FD           -- release persistent pin for one dma-buf
 *
 * Consumer (remote GPU importer):
 *   VMEM_IOCTL_GET_IPC_FD           -- build fake dma-buf from offset list
 *
 * Utilities:
 *   VMEM_IOCTL_VERSION              -- query driver version
 *   VMEM_IOCTL_IDENTIFY_DMABUF     -- probe which GPU owns a dma-buf
 *
 * Changes from v0.1:
 *   - pfn_list no longer embedded in ioctl args (userspace pointer instead)
 *   - GET_IPC_FD takes consumer_bus/device/function (no REGISTER_EP needed)
 *   - CLOSE_IPC_FD removed: call close(fd) from userspace
 *   - REGISTER_EP removed: consumer BDF now per-call
 *   - ABS_PFN flag for direct-addressable fabrics
 *   - Dynamic entry count (ENOSPC + retry)
 *   - _IOWR encoding with proper struct sizes
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

#define VMEM_DRV_VERSION_MAJOR 1
#define VMEM_DRV_VERSION_MINOR 0
#define VMEM_DRV_VERSION_PATCH 0

/* Recommended userspace buffer size for pfn_entries[] (not a hard kernel limit) */
#define VMEM_MAX_PFN_ENTRIES 4096

/* Flags for VMEM_IOCTL_GET_PFN_OFFSET_LIST */
#define VMEM_GET_PFN_FLAG_ABS_PA  (1 << 0)  /* return absolute physical addr instead of BAR-relative offset */

/* Flags for VMEM_IOCTL_GET_IPC_FD */
#define VMEM_IPC_FLAG_ABS_PA      (1 << 0)  /* entries contain absolute physical addresses */

/**
 * struct vmem_pfn_entry - one scatter entry for cross-node PFN transport
 * @offset: BAR2-relative byte offset (or absolute physical addr if ABS_PA flag)
 * @size:   byte length of this scatter chunk (page-aligned)
 */
struct vmem_pfn_entry {
	__u64 offset;
	__u32 size;
	__u32 pad;
};

/* ── IOCTL argument structures ───────────────────────────── */

/**
 * struct vmem_ioctl_version_arg - VMEM_IOCTL_VERSION
 */
struct vmem_ioctl_version_arg {
	__u16 major;
	__u16 minor;
	__u16 patch;
	__u16 reserved;
};

/**
 * struct vmem_ioctl_get_pfn_arg - VMEM_IOCTL_GET_PFN_OFFSET_LIST
 *
 * Producer: attach to GPU dma-buf, extract BAR-relative scatter offsets.
 * @count IN = capacity of entries_ptr[]; OUT = actual entry count.
 * Returns -ENOSPC (with count = required) if buffer too small → retry.
 * Driver keeps attachment alive until PUT_IPC_FD or vmem fd close.
 */
struct vmem_ioctl_get_pfn_arg {
	__s32  fd;           /* in:  GPU dma-buf fd (from zeMemGetIpcHandle) */
	__u8   bus;          /* in:  GPU PCIe bus */
	__u8   device;       /* in:  GPU PCIe device */
	__u8   function;     /* in:  GPU PCIe function */
	__u8   flags;        /* in:  VMEM_GET_PFN_FLAG_* */
	__u32  count;        /* in/out: capacity / actual entry count */
	__u32  pad;
	__u64  entries_ptr;  /* in:  userspace ptr → vmem_pfn_entry[] output buffer */
};

/**
 * struct vmem_ioctl_get_ipc_fd_arg - VMEM_IOCTL_GET_IPC_FD
 *
 * Consumer: build a fake dma-buf from received scatter entries.
 * consumer_bus/device/function: the local GPU that will import this buffer.
 * Driver resolves consumer BAR2 base via pci_resource_start().
 */
struct vmem_ioctl_get_ipc_fd_arg {
	__u8   consumer_bus;       /* in:  consumer GPU PCIe bus */
	__u8   consumer_device;    /* in:  consumer GPU PCIe device */
	__u8   consumer_function;  /* in:  consumer GPU PCIe function */
	__u8   flags;              /* in:  VMEM_IPC_FLAG_* */
	__u32  count;              /* in:  number of entries in entries_ptr[] */
	__u64  entries_ptr;        /* in:  userspace ptr → vmem_pfn_entry[] from producer */
	__s32  fd;                 /* out: fake dma-buf fd */
	__u32  pad;
};

/**
 * struct vmem_ioctl_put_ipc_fd_arg - VMEM_IOCTL_PUT_IPC_FD
 *
 * Release the persistent dma-buf attachment kept since GET_PFN_OFFSET_LIST.
 * @fd: the original GPU dma-buf fd passed to GET_PFN_OFFSET_LIST.
 */
struct vmem_ioctl_put_ipc_fd_arg {
	__s32 fd;
	__u32 pad;
};

/**
 * struct vmem_ioctl_identify_dmabuf_arg - VMEM_IOCTL_IDENTIFY_DMABUF
 *
 * Probe which GPU's VRAM backs a dma-buf: temporary attach via @bus/@device/@function,
 * read first DMA address, scan PCI BARs, return source GPU BDF.
 */
struct vmem_ioctl_identify_dmabuf_arg {
	__s32  fd;               /* in:  dma-buf fd to probe */
	__u8   bus;              /* in:  PCI bus for temporary attach */
	__u8   device;           /* in:  PCI device */
	__u8   function;         /* in:  PCI function */
	__u8   pad;
	__u8   source_bus;       /* out: source GPU bus */
	__u8   source_device;    /* out: source GPU device */
	__u8   source_function;  /* out: source GPU function */
	__u8   pad2;
};

/* ── IOCTL commands ──────────────────────────────────────── */

#define VMEM_IOCTL_VERSION \
	_IOR(VMEM_MAGIC, 0x00, struct vmem_ioctl_version_arg)

#define VMEM_IOCTL_GET_PFN_OFFSET_LIST \
	_IOWR(VMEM_MAGIC, 0x01, struct vmem_ioctl_get_pfn_arg)

#define VMEM_IOCTL_GET_IPC_FD \
	_IOWR(VMEM_MAGIC, 0x02, struct vmem_ioctl_get_ipc_fd_arg)

/**
 * struct vmem_ioctl_close_ipc_fd_arg - VMEM_IOCTL_CLOSE_IPC_FD
 *
 * Consumer calls this to destroy the fake dma-buf and release kernel resources.
 * The driver closes the fd on behalf of the caller via close_fd().
 */
struct vmem_ioctl_close_ipc_fd_arg {
	__s32 fd;
	__u32 pad;
};

#define VMEM_IOCTL_CLOSE_IPC_FD \
	_IOW(VMEM_MAGIC, 0x03, struct vmem_ioctl_close_ipc_fd_arg)

#define VMEM_IOCTL_PUT_IPC_FD \
	_IOW(VMEM_MAGIC, 0x04, struct vmem_ioctl_put_ipc_fd_arg)

/* 0x05: VMEM_IOCTL_REGISTER_EP removed — consumer BDF now per-call in GET_IPC_FD */

#define VMEM_IOCTL_IDENTIFY_DMABUF \
	_IOWR(VMEM_MAGIC, 0x06, struct vmem_ioctl_identify_dmabuf_arg)

#endif /* _UAPI_VMEM_IOCTL_H */
