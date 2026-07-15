# vMem KMD — PCIe Cross-Node GPU dma-buf Translator

**Version:** 2.1.0 | **License:** GPL-2.0 | **Device:** `/dev/vmemIntel`

## Overview

vMem is a Linux kernel module that enables cross-node GPU memory sharing over a PCIe Gen5
switch (Astera PEX890xx). It operates on the **consumer (peer) side only**: given a set of
BAR-relative scatter offsets forwarded from the origin node, it resolves the corresponding
physical addresses in-kernel via the Astera vFE driver and exports an MMIO-backed pseudo
dma-buf for the local GPU to use for P2P DMA.

### In-Kernel PA Synthesis (Method 1)

```
vmem_get_dmabuf_fd input: {node_id, gpu_id, offset list}

KMD flow:
  astera_lookup(node_id, gpu_id)
    -> (dbdf, bar, index)
    -> base = pci_resource_start(dbdf, bar) + index × 32 GB
    -> PA'[i] = base + offset[i]
    -> build MMIO dma-buf
```

```
Origin Node                              Peer Node
──────────────────────────────────────   ─────────────────────────────────────
GPU App                                  GPU App
  │ allocate VRAM                          │ import pseudo dma-buf → P2P R/W
  │ zeMemGetIpcHandle()                    ↑
  │ extract BAR-relative offsets           │ zeMemOpenIpcHandle(pseudo_fd)
  │   (via origin-side tooling)            │
  ↓                                        │
  ·── control channel ─────────────────→  vMem KMD (/dev/vmemIntel)
     {node_id, gpu_id, offset list}         │ VMEM_IOCTL_GET_IPC_HANDLE
                                            │   vmem_astera_lookup(node_id, gpu_id)
                                            │     -> (dbdf, bar, index)
                                            │   base = BAR_start + index × 32 GB
                                            │   PA'[i] = base + offset[i]
                                            │   dma_buf_export() -> pseudo_fd
                                            ↓
                                          Astera vFE driver (astera_vfe.ko)
                                          registered via vmem_register_astera_lookup()
```

### Layer Responsibilities

| Layer | Role |
|-------|------|
| **vmem KMD** | Receive `{node_id, gpu_id, offsets}`, call astera_lookup, compute PAs, export pseudo dma-buf |
| **vmem_astera stub** | Function-pointer registration; returns `-ENOSYS` until `astera_vfe.ko` registers |
| **astera_vfe.ko** | Registers `astera_lookup(node_id, gpu_id)` → `(dbdf, bar, index)` |
| **Origin tooling / daemon** | Produces BAR-relative offsets; forwards `{node_id, gpu_id, offsets}` over control channel |

### Status

| Mode | Result | Notes |
|------|--------|-------|
| Consumer path (PA math) | **PASS** | astera_lookup stub wired; PA formula verified |
| Real cross-node P2P | Pending | Requires `astera_vfe.ko` + Astera switch BAR-to-BAR routing |

---

## Hardware (Verified)

```
Peer GPU:    BDF 3f:00.0  Intel Arc e211  BAR2 @ 0x22e800000000  32 GiB
PCIe Switch: BDF 09:00.0  Broadcom/LSI PEX890xx PCIe Gen5
Kernel:      6.14.0-36-generic
GPU driver:  Intel XE (xe.ko, in-kernel)
Level Zero:  libze_loader.so.1
```

> **IOMMU:** Set `intel_iommu=pt` (passthrough) so DMA addresses equal physical addresses.

---

## Repository Layout

```
driver.gpu.vmem/
├── include/
│   └── vmem_ioctl.h        UAPI — include in UMD
├── vmem_astera.h           Astera vFE interface: struct astera_vfe_ep, registration API
├── vmem_astera.c           Registration stub; exports vmem_register/unregister_astera_lookup
├── vmem_dmabuf.h           Internal types (vmem_buf) + vmem_get_ipc_handle declaration
├── vmem_drv.c              Module init, /dev/vmemIntel misc device, ioctl dispatch
├── vmem_dmabuf.c           vmem_get_ipc_handle() + vmem_identify_dmabuf_source()
├── Makefile
├── README.md
└── test/
    ├── vmem_gpu_test.c     Full GPU integration test (Level Zero + Arc e211)
    └── vmem_test.c         Basic ioctl smoke test
```

---

## Build

```bash
apt install linux-headers-$(uname -r) libze-dev level-zero

make              # builds vmem.ko
make install      # insmod vmem.ko  ->  /dev/vmemIntel
make uninstall    # rmmod vmem
```

---

## IOCTL API

Include `include/vmem_ioctl.h`. Magic number: `'V'` (0x56).

### Commands

| Command | Code | Dir | Description |
|---------|------|-----|-------------|
| `VMEM_IOCTL_VERSION` | 0x00 | `_IOR` | Query driver version |
| `VMEM_IOCTL_GET_IPC_HANDLE` | 0x02 | `_IOWR` | Build pseudo dma-buf via in-kernel Astera vFE lookup |
| `VMEM_IOCTL_PUT_IPC_HANDLE` | 0x03 | `_IOW` | Destroy pseudo dma-buf fd |
| `VMEM_IOCTL_IDENTIFY_DMABUF` | 0x06 | `_IOWR` | Probe which GPU's VRAM backs a dma-buf |

Codes 0x01 and 0x04 are reserved (previously origin-side ioctls, removed in v2.1).

### Argument Structures

```c
/* vmem_pfn_entry — one scatter chunk.
 * offset: BAR-relative byte offset from origin GPU VRAM base
 *         (KMD adds vFE window base to compute absolute PA)
 * size:   page-aligned byte length */
struct vmem_pfn_entry {
    __u64 offset;
    __u32 size;
    __u32 pad;
};

/* GET_IPC_HANDLE */
struct vmem_ioctl_get_ipc_handle_arg {
    __u32  node_id;      /* in:  remote node identifier */
    __u32  gpu_id;       /* in:  remote GPU identifier on that node */
    __u32  count;        /* in:  number of entries */
    __u32  pad;
    __u64  entries_ptr;  /* in:  userspace ptr -> vmem_pfn_entry[] (BAR-relative) */
    __s32  fd;           /* out: pseudo dma-buf fd */
    __u32  pad2;
};

/* PUT_IPC_HANDLE */
struct vmem_ioctl_put_ipc_handle_arg {
    __s32 fd;   /* pseudo dma-buf fd to close */
    __u32 pad;
};
```

---

## Usage

```c
int vmem_fd = open("/dev/vmemIntel", O_RDWR);

/* Receive from origin node via control channel */
struct vmem_pfn_entry *entries;
uint32_t count, node_id, gpu_id;
recv_from_origin(&entries, &count, &node_id, &gpu_id);

/*
 * GET_IPC_HANDLE: KMD calls astera_lookup(node_id, gpu_id)
 *   -> (dbdf, bar, index) -> base = BAR_start + index*32GB
 *   -> PA'[i] = base + entries[i].offset
 *   -> pseudo dma-buf fd
 */
struct vmem_ioctl_get_ipc_handle_arg arg = {
    .node_id     = node_id,
    .gpu_id      = gpu_id,
    .count       = count,
    .entries_ptr = (uint64_t)(uintptr_t)entries,
};
if (ioctl(vmem_fd, VMEM_IOCTL_GET_IPC_HANDLE, &arg) < 0) {
    /* -ENOSYS: astera_vfe.ko not loaded */
    perror("GET_IPC_HANDLE"); goto out;
}

/* Import pseudo dma-buf into local GPU */
ze_ipc_mem_handle_t ipc = {};
memcpy(ipc.data, &arg.fd, sizeof(int));
void *remote_ptr;
zeMemOpenIpcHandle(ctx, peer_gpu, ipc, ZE_IPC_MEMORY_FLAG_BIAS_UNCACHED, &remote_ptr);

/* P2P DMA — Astera switch routes accesses to origin VRAM */

/* Teardown */
zeMemCloseIpcHandle(ctx, remote_ptr);
struct vmem_ioctl_put_ipc_handle_arg put = { .fd = arg.fd };
ioctl(vmem_fd, VMEM_IOCTL_PUT_IPC_HANDLE, &put);
out:
free(entries);
close(vmem_fd);
```

---

## Astera vFE Driver Integration

`vmem_astera.c` provides a registration stub so `vmem.ko` has no hard link-time dependency
on `astera_vfe.ko`.

### Module load order

```
vmem.ko         loads first  — exports vmem_register_astera_lookup
astera_vfe.ko   loads second — calls vmem_register_astera_lookup(my_fn)
astera_vfe.ko   unloads      — calls vmem_unregister_astera_lookup(my_fn)
```

`GET_IPC_HANDLE` returns `-ENOSYS` until `astera_vfe.ko` registers. All other ioctls work
independently.

### `astera_vfe.ko` implementation template

```c
#include <vmem_astera.h>

static int my_astera_lookup(u32 node_id, u32 gpu_id, struct astera_vfe_ep *ep)
{
    struct my_vfe_record *rec = vfe_table_lookup(node_id, gpu_id);
    if (!rec)
        return -ENODEV;

    ep->domain = rec->pci_domain;
    ep->bus    = rec->pci_bus;
    ep->devfn  = PCI_DEVFN(rec->pci_slot, rec->pci_fn);
    ep->bar    = 2;               /* BAR2 = LMEM window on PEX890xx */
    ep->index  = rec->window_idx; /* 32 GiB slot within BAR2 */
    return 0;
}

static int __init astera_vfe_init(void)
{
    vmem_register_astera_lookup(my_astera_lookup);
    return 0;
}
static void __exit astera_vfe_exit(void)
{
    vmem_unregister_astera_lookup(my_astera_lookup);
}
module_init(astera_vfe_init);
module_exit(astera_vfe_exit);
MODULE_LICENSE("GPL");
```

### `struct astera_vfe_ep` fields

| Field | Meaning |
|-------|---------|
| `domain` | PCI domain of the vFE device on the peer host |
| `bus` | PCI bus number |
| `devfn` | `PCI_DEVFN(slot, fn)` |
| `bar` | BAR index (2 for LMEM window on PEX890xx) |
| `index` | Window slot; `base = BAR_start + index × 32 GiB` |

---

## Design Notes

### PA synthesis in the KMD, not the daemon

The daemon's only job is to forward `{node_id, gpu_id, BAR-relative offsets}` over a
control channel. No address computation happens in userspace.

The KMD calls `vmem_astera_lookup()` — a spinlock-protected function pointer — to resolve
the vFE endpoint, then computes `base + offset` in-kernel. This keeps the hot path
kernel-only and allows `astera_vfe.ko` to be updated or replaced without touching vmem KMD.

### Dynamic scatter count

The origin node produces as many `vmem_pfn_entry` entries as the GPU buffer's sg_table
contains. There is no hard kernel limit; the peer passes the actual count in `GET_IPC_HANDLE`.

---

## Changelog

### v2.1.0

- **Removed origin side**: `OPEN_IPC_HANDLE`, `CLOSE_IPC_HANDLE`, `vmem_open_ipc_handle()`,
  `vmem_import_pin`, `vmem_file_priv`, soft-pin machinery — all deleted
- **`vmem_get_ipc_handle()`**: renamed from `vmem_get_ipc_handle_method1`; now the single
  public function in `vmem_dmabuf.c`
- **`vmem_drv.c`**: no per-fd state (`open`/`release` fops removed); dispatch table reduced
  to four ioctls
- **UAPI**: codes 0x01 and 0x04 marked reserved; only `GET_IPC_HANDLE` (0x02) and
  `PUT_IPC_HANDLE` (0x03) active

### v2.0.0

- PA synthesis moved from userspace daemon into KMD (Method 1)
- New `vmem_astera.h/c` registration stub
- Renamed ioctls to `open/get/put/close_ipc_handle`

### v1.0.0

- `GET_IPC_FD`: PAs supplied by daemon; no BDF in kernel
- `GET_PFN_OFFSET_LIST`: BAR-relative offsets; userspace pointer; `-ENOSPC` retry
- Per-fd persistent soft-pin; `PUT_IPC_FD` releases pin

### v0.1.0

Initial: global `REGISTER_EP` table, fixed entry limit, consumer BAR resolved in kernel.
