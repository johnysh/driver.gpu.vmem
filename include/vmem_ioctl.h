/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vmem_ioctl.h - vMem driver UAPI: shared by kernel module and userspace
 *
 * vMem acts as a dma-buf translator for cross-node GPU P2P over PCIe switch.
 *
 * Producer side (local GPU owner):
 *   VMEM_IOCTL_GET_PFN_OFFSET_LIST  -- parse real dma-buf, return PFN offsets
 *   VMEM_IOCTL_PUT_IPC_FD           -- release exported GPU address info
 *
 * Consumer side (remote GPU importer):
 *   VMEM_IOCTL_GET_IPC_FD           -- build fake dma-buf from PFN offsets
 *   VMEM_IOCTL_CLOSE_IPC_FD         -- destroy the fake dma-buf
 *
 * Endpoint registration (PCIe SW driver / daemon):
 *   VMEM_IOCTL_REGISTER_EP          -- register local PCIe endpoint BAR2
 */

#ifndef _UAPI_VMEM_IOCTL_H
#define _UAPI_VMEM_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#include <asm/types.h>  /* __u8 __u16 __u32 __u64 __s32 */
#endif

#define VMEM_MAGIC 'V'

/* Maximum number of scatter entries per buffer */
#define VMEM_MAX_PFN_ENTRIES 4096

/**
 * struct vmem_pfn_entry - single scatter-list entry for cross-node transfer
 * @offset: byte offset of this chunk from GPU BAR2 base address
 * @size:   byte length of this chunk (must be page-aligned)
 */
struct vmem_pfn_entry {
	__u64 offset;
	__u32 size;
	__u32 pad;
};

/**
 * struct vmem_pfn_list - ordered list of PFN scatter entries for one buffer
 * @count:   number of valid entries
 * @entries: scatter entries
 */
struct vmem_pfn_list {
	__u32 count;
	__u32 pad;
	struct vmem_pfn_entry entries[VMEM_MAX_PFN_ENTRIES];
};

/* -- IOCTL argument structures -------------------- */

/**
 * struct vmem_ioctl_get_pfn_arg - VMEM_IOCTL_GET_PFN_OFFSET_LIST
 *
 * Producer calls this to extract PFN offsets from a real dma-buf fd.
 * The driver attaches to the dma-buf via the GPU identified by (bus,dev,fn),
 * maps the scatter list, computes offset = dma_addr - GPU_BAR2_base for each
 * entry, and returns the offset list to userspace.
 */
struct vmem_ioctl_get_pfn_arg {
	__s32 fd;         /* in:  dma-buf fd from zeMemGetIpcHandle */
	__u8  bus;        /* in:  GPU PCIe bus number               */
	__u8  device;     /* in:  GPU PCIe device number            */
	__u8  function;   /* in:  GPU PCIe function number          */
	__u8  pad;
	struct vmem_pfn_list pfn_list; /* out: PFN offset list     */
};

/**
 * struct vmem_ioctl_get_ipc_fd_arg - VMEM_IOCTL_GET_IPC_FD
 *
 * Consumer calls this to create a "fake" dma-buf from received PFN offsets.
 * The driver translates each offset through the registered local endpoint
 * BAR2 base (offset + ep_bar2_base = local physical address), assembles a
 * synthetic dma-buf, exports it, and returns the fd.
 */
struct vmem_ioctl_get_ipc_fd_arg {
	struct vmem_pfn_list pfn_list; /* in:  PFN list from producer */
	__s32 fd;                      /* out: fake dma-buf fd        */
	__u32 pad;
};

/**
 * struct vmem_ioctl_close_ipc_fd_arg - VMEM_IOCTL_CLOSE_IPC_FD
 *
 * Consumer calls this to destroy the fake dma-buf and release resources.
 * Equivalent to close(fd) on the fake dma-buf fd.
 */
struct vmem_ioctl_close_ipc_fd_arg {
	__s32 fd;
	__u32 pad;
};

/**
 * struct vmem_ioctl_put_ipc_fd_arg - VMEM_IOCTL_PUT_IPC_FD
 *
 * Producer calls this to release any reference the driver holds on the
 * original dma-buf fd after GET_PFN_OFFSET_LIST.
 */
struct vmem_ioctl_put_ipc_fd_arg {
	__s32 fd;
	__u32 pad;
};

/**
 * struct vmem_ioctl_register_ep_arg - VMEM_IOCTL_REGISTER_EP
 *
 * PCIe SW driver / daemon calls this to register the local virtual/physical
 * PCIe endpoint that maps to the remote GPU VRAM via the L2 switch.
 * If bar2_base == 0 the driver auto-detects BAR2 from the BDF.
 */
struct vmem_ioctl_register_ep_arg {
	__u8  bus;
	__u8  device;
	__u8  function;
	__u8  pad;
	__u32 pad2;
	__u64 bar2_base; /* physical address; 0 = auto-detect from BDF */
	__u64 bar2_size; /* BAR2 window size in bytes                  */
};

/* -- IOCTL command codes -------------------- */

#define VMEM_IOCTL_GET_PFN_OFFSET_LIST	_IO(VMEM_MAGIC, 0x01)

#define VMEM_IOCTL_GET_IPC_FD		_IO(VMEM_MAGIC, 0x02)

#define VMEM_IOCTL_CLOSE_IPC_FD \
	_IOW(VMEM_MAGIC, 0x03, struct vmem_ioctl_close_ipc_fd_arg)

#define VMEM_IOCTL_PUT_IPC_FD \
	_IOW(VMEM_MAGIC, 0x04, struct vmem_ioctl_put_ipc_fd_arg)

#define VMEM_IOCTL_REGISTER_EP \
	_IOW(VMEM_MAGIC, 0x05, struct vmem_ioctl_register_ep_arg)

/* -- Return codes -------------------- */
#define BMEMLINK_OK    0
#define BMEMLINK_ERROR (-1)

#endif /* _UAPI_VMEM_IOCTL_H */

