# vMem KMD — PCIe Cross-Node GPU dma-buf Translator

**Version:** 2.0.0 | **License:** GPL-2.0 | **Device:** `/dev/vmemIntel`

## Overview

vMem is a Linux kernel module that enables cross-node GPU memory sharing over a PCIe Gen5
switch (Astera PEX890xx). It translates GPU dma-buf handles between nodes using the
**Astera vFE (virtual Fabric Endpoint)** MMIO window as the physical address bridge.

### v2.0 Architecture: Method 1 — In-Kernel PA Synthesis

Physical address synthesis now runs **entirely inside the KMD**. The peer node passes
`{node_id, gpu_id, BAR-relative offsets}` to `VMEM_IOCTL_GET_IPC_HANDLE`; the kernel
resolves the vFE endpoint via a registered Astera callback and computes absolute PAs
without any daemon involvement in the hot path.

```
Origin Node                              Peer Node
──────────────────────────────────────   ─────────────────────────────────────
GPU App                                  GPU App
  │ allocate VRAM                          │ import pseudo dma-buf → P2P R/W
  ↓                                        ↑
xe GPU driver                            xe GPU driver
  │ dma-buf fd                             │ pseudo dma-buf fd
  ↓                                        │
vMem KMD (/dev/vmemIntel)                vMem KMD (/dev/vmemIntel)
  │ OPEN_IPC_HANDLE(dmabuf_fd, bdf)        │ GET_IPC_HANDLE(node_id, gpu_id, offsets)
  │   dma_buf_get → P2P attach             │   vmem_astera_lookup(node_id, gpu_id)
  │   sg_table → BAR-relative offsets      │     → (dbdf, bar, index)
  │   soft-pin (keep attachment alive)     │   base = BAR_start + index × 32 GiB
  ↓                                        │   PA'  = base + offset   [per scatter]
vMem UMD / daemon                          │   build MMIO vmem_buf → dma_buf_export()
  │ {offsets, gpu_id, node_id}  ────────→  └──── pseudo dma-buf fd
  └──── control path (any IPC channel)
                                                    ↑
                                          vmem_astera.c (registration stub)
                                            ← vmem_register_astera_lookup()
                                          astera_vfe.ko (when present)
```

### Layer Responsibilities

| Layer | Role |
|-------|------|
| **vmem KMD** | Origin: extract BAR-relative scatter offsets; Peer: call astera_lookup, compute PAs, build pseudo dma-buf |
| **vmem_astera stub** | Function-pointer registration; returns `-ENOSYS` until `astera_vfe.ko` registers |
| **astera_vfe.ko** | Registers `astera_lookup(node_id, gpu_id)` → `(dbdf, bar, index)`; implements vFE endpoint table |
| **Daemon / UMD** | Transport `{offsets, gpu_id, node_id}` over control channel; no PA math required |

### Status

| Mode | Result | Notes |
|------|--------|-------|
| Single-machine (2 GPUs, 1 host) | **PASS** | All ioctls verified, PA math correct |
| Real cross-node P2P | Pending | Requires `astera_vfe.ko` + Astera switch BAR-to-BAR routing |

---

## Hardware (Verified)

```
GPU0 (origin): BDF 39:00.0  Intel Arc e211  BAR2 @ 0x22f000000000  32 GiB
GPU1 (peer):   BDF 3f:00.0  Intel Arc e211  BAR2 @ 0x22e800000000  32 GiB
PCIe Switch:   BDF 09:00.0  Broadcom/LSI PEX890xx PCIe Gen5
Kernel:        6.14.0-36-generic
GPU driver:    Intel XE (xe.ko, in-kernel)
Level Zero:    libze_loader.so.1
```

> **IOMMU:** Set `intel_iommu=pt` (passthrough) so DMA addresses equal physical addresses.
> Without this, cross-node PFN transport is invalid.

---

## Repository Layout

```
driver.gpu.vmem/
├── include/
│   └── vmem_ioctl.h        UAPI — include in both KMD and UMD
├── vmem_astera.h           Astera vFE interface: struct astera_vfe_ep, registration API
├── vmem_astera.c           Registration stub; exports vmem_register/unregister_astera_lookup
├── vmem_dmabuf.h           Internal types (vmem_buf, vmem_import_pin, vmem_file_priv)
├── vmem_drv.c              Module init, /dev/vmemIntel misc device, ioctl dispatch
├── vmem_dmabuf.c           Origin parse, peer Method-1 build, identify, pin lifecycle
├── Makefile
├── README.md
└── test/
    ├── vmem_gpu_test.c     Full GPU integration test (Level Zero + 2x Arc e211)
    └── vmem_test.c         Basic ioctl smoke test (no GPU needed)
```

---

## Build

```bash
# Prerequisites
apt install linux-headers-$(uname -r) libze-dev level-zero

# Kernel module
make              # builds vmem.ko  (includes vmem_astera.o)
make install      # insmod vmem.ko  ->  /dev/vmemIntel
make uninstall    # rmmod vmem

# Integration test
cd test
gcc -O2 -Wall -g -I.. -o vmem_gpu_test vmem_gpu_test.c -lze_loader -ldl
```

---

## IOCTL API

Include `include/vmem_ioctl.h`. Magic number: `'V'` (0x56).

### Commands

| Command | Code | Dir | Side | Description |
|---------|------|-----|------|-------------|
| `VMEM_IOCTL_VERSION` | 0x00 | `_IOR` | — | Query driver version |
| `VMEM_IOCTL_OPEN_IPC_HANDLE` | 0x01 | `_IOWR` | Origin | Attach GPU dma-buf, return BAR-relative scatter offsets; stores soft-pin |
| `VMEM_IOCTL_GET_IPC_HANDLE` | 0x02 | `_IOWR` | Peer | Method 1: KMD calls `astera_lookup`, computes PAs, returns pseudo dma-buf fd |
| `VMEM_IOCTL_PUT_IPC_HANDLE` | 0x03 | `_IOW` | Peer | Destroy pseudo dma-buf fd |
| `VMEM_IOCTL_CLOSE_IPC_HANDLE` | 0x04 | `_IOW` | Origin | Release persistent soft-pin |
| `VMEM_IOCTL_IDENTIFY_DMABUF` | 0x06 | `_IOWR` | — | Probe which GPU's VRAM backs a dma-buf |

**Backward compat aliases** (v1.0 names, same ioctl numbers):

```c
#define VMEM_IOCTL_GET_PFN_OFFSET_LIST  VMEM_IOCTL_OPEN_IPC_HANDLE
#define VMEM_IOCTL_PUT_IPC_FD           VMEM_IOCTL_CLOSE_IPC_HANDLE
#define VMEM_IOCTL_CLOSE_IPC_FD         VMEM_IOCTL_PUT_IPC_HANDLE
```

### Core Types

```c
/* Scatter entry */
struct vmem_pfn_entry {
    __u64 offset;   /* OPEN_IPC_HANDLE out: BAR2-relative byte offset
                       GET_IPC_HANDLE  in:  same BAR-relative offset (KMD computes PA) */
    __u32 size;     /* page-aligned byte length */
    __u32 pad;
};

/* OPEN_IPC_HANDLE — origin */
struct vmem_ioctl_open_ipc_handle_arg {
    __s32  fd;           /* in:  GPU dma-buf fd */
    __u8   bus;          /* in:  GPU PCIe bus   */
    __u8   device;       /* in:  GPU PCIe device */
    __u8   function;     /* in:  GPU PCIe function */
    __u8   pad2;
    __u32  count;        /* in/out: capacity / actual; -ENOSPC if too small → retry */
    __u32  pad;
    __u64  entries_ptr;  /* in:  userspace ptr → vmem_pfn_entry[] output */
};

/* GET_IPC_HANDLE — peer (Method 1) */
struct vmem_ioctl_get_ipc_handle_arg {
    __u32  node_id;      /* in:  remote node identifier */
    __u32  gpu_id;       /* in:  remote GPU identifier on that node */
    __u32  count;        /* in:  number of entries */
    __u32  pad;
    __u64  entries_ptr;  /* in:  userspace ptr → vmem_pfn_entry[] (BAR-relative offsets) */
    __s32  fd;           /* out: pseudo dma-buf fd */
    __u32  pad2;
};

/* PUT_IPC_HANDLE — peer teardown */
struct vmem_ioctl_put_ipc_handle_arg { __s32 fd; __u32 pad; };

/* CLOSE_IPC_HANDLE — origin teardown */
struct vmem_ioctl_close_ipc_handle_arg { __s32 fd; __u32 pad; };
```

---

## Usage

### 1. Origin Node

```c
int vmem_fd = open("/dev/vmemIntel", O_RDWR);

/* Export GPU buffer as dma-buf */
ze_ipc_mem_handle_t ipc_h;
zeMemGetIpcHandle(ctx, gpu_ptr, &ipc_h);
int dma_fd;
memcpy(&dma_fd, ipc_h.data, sizeof(int));

/* Extract BAR-relative offsets (-ENOSPC retry pattern) */
uint32_t count = VMEM_MAX_PFN_ENTRIES;
struct vmem_pfn_entry *entries = calloc(count, sizeof(*entries));

retry:;
struct vmem_ioctl_open_ipc_handle_arg arg = {
    .fd = dma_fd, .bus = BUS, .device = DEV, .function = FN,
    .count = count, .entries_ptr = (uint64_t)(uintptr_t)entries,
};
if (ioctl(vmem_fd, VMEM_IOCTL_OPEN_IPC_HANDLE, &arg) < 0 && errno == ENOSPC) {
    count = arg.count;
    entries = realloc(entries, count * sizeof(*entries));
    goto retry;
}
/* arg.count = actual BAR-relative entries */

/* Send {entries, count, gpu_id, node_id} to peer via control channel */
send_to_peer(entries, arg.count, MY_GPU_ID, MY_NODE_ID);

/* Release pin when peer is done */
struct vmem_ioctl_close_ipc_handle_arg close_arg = { .fd = dma_fd };
ioctl(vmem_fd, VMEM_IOCTL_CLOSE_IPC_HANDLE, &close_arg);
zeMemPutIpcHandle(ctx, ipc_h);
free(entries);
close(vmem_fd);
```

### 2. Peer Node

```c
int vmem_fd = open("/dev/vmemIntel", O_RDWR);

/* Receive BAR-relative entries + IDs from origin via control channel */
struct vmem_pfn_entry *entries;
uint32_t count, node_id, gpu_id;
recv_from_origin(&entries, &count, &node_id, &gpu_id);

/*
 * GET_IPC_HANDLE (Method 1):
 *   KMD calls vmem_astera_lookup(node_id, gpu_id) -> (dbdf, bar, index)
 *   base = pci_resource_start(vfe_pdev, bar) + index * 32 GiB
 *   PA'[i] = base + entries[i].offset
 *   Returns pseudo dma-buf backed by MMIO window into origin VRAM.
 */
struct vmem_ioctl_get_ipc_handle_arg arg = {
    .node_id     = node_id,
    .gpu_id      = gpu_id,
    .count       = count,
    .entries_ptr = (uint64_t)(uintptr_t)entries,
};
if (ioctl(vmem_fd, VMEM_IOCTL_GET_IPC_HANDLE, &arg) < 0) {
    /* -ENOSYS: astera_vfe.ko not loaded yet */
    perror("GET_IPC_HANDLE");
    goto out;
}
int pseudo_fd = arg.fd;

/* Import pseudo dma-buf into peer GPU for P2P DMA */
ze_ipc_mem_handle_t pseudo_ipc = {};
memcpy(pseudo_ipc.data, &pseudo_fd, sizeof(int));
void *remote_ptr;
zeMemOpenIpcHandle(ctx, peer_gpu, pseudo_ipc,
                   ZE_IPC_MEMORY_FLAG_BIAS_UNCACHED, &remote_ptr);
/* P2P DMA via remote_ptr — Astera switch routes to origin VRAM */

/* Teardown */
zeMemCloseIpcHandle(ctx, remote_ptr);
struct vmem_ioctl_put_ipc_handle_arg put_arg = { .fd = pseudo_fd };
ioctl(vmem_fd, VMEM_IOCTL_PUT_IPC_HANDLE, &put_arg);
out:
free(entries);
close(vmem_fd);
```

### 3. Daemon (control path only)

In v2.0 the daemon no longer does PA synthesis. Its only job is to forward
`{offsets, count, gpu_id, node_id}` from origin to peer over whatever IPC channel
is available (Unix socket, RDMA, etc.).

```c
/* Origin side: receive from KMD, forward to peer */
recv_entries_from_origin_kmd(&entries, &count);
send_to_peer_daemon(entries, count, origin_gpu_id, origin_node_id);

/* Peer side: receive and pass directly to KMD */
recv_from_origin_daemon(&entries, &count, &gpu_id, &node_id);
pass_to_peer_kmd(entries, count, gpu_id, node_id);  /* -> GET_IPC_HANDLE */
```

---

## Astera vFE Driver Integration

`vmem_astera.c` provides a **registration stub** that decouples vmem.ko from
`astera_vfe.ko` at link time.

### Module load order

```
vmem.ko           loads first — exports vmem_register_astera_lookup
astera_vfe.ko     loads second — calls vmem_register_astera_lookup(my_fn)
astera_vfe.ko     unloads — calls vmem_unregister_astera_lookup(my_fn)
```

`GET_IPC_HANDLE` returns `-ENOSYS` until `astera_vfe.ko` registers a callback. All other
ioctls (`OPEN/CLOSE/PUT_IPC_HANDLE`, `VERSION`, `IDENTIFY_DMABUF`) work independently.

### astera_vfe.ko implementation template

```c
#include <vmem_astera.h>   /* from vmem KMD headers */

static int my_astera_lookup(u32 node_id, u32 gpu_id, struct astera_vfe_ep *ep)
{
    /* Query Astera SDK / internal table for (node_id, gpu_id) */
    struct my_vfe_record *rec = vfe_table_lookup(node_id, gpu_id);
    if (!rec)
        return -ENODEV;

    ep->domain = rec->pci_domain;
    ep->bus    = rec->pci_bus;
    ep->devfn  = PCI_DEVFN(rec->pci_dev, rec->pci_fn);
    ep->bar    = 2;               /* BAR2 = LMEM window on PEX890xx */
    ep->index  = rec->window_idx; /* 32 GiB slot index within BAR2   */
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

| Field | Type | Meaning |
|-------|------|---------|
| `domain` | `u16` | PCI domain of the vFE device on the peer host |
| `bus` | `u8` | PCI bus |
| `devfn` | `u8` | `PCI_DEVFN(slot, fn)` |
| `bar` | `u8` | BAR index (2 for LMEM window on PEX890xx) |
| `index` | `u32` | Window slot; `base = BAR_start + index × 32 GiB` |

---

## Soft-Pin Lifecycle

After `OPEN_IPC_HANDLE` succeeds, the KMD stores `{dmabuf, attach, sgt, pdev}` in a
per-fd `vmem_import_pin` list. This keeps XE TTM from evicting the VRAM BO while the
peer holds the offset list.

| Event | Action |
|-------|--------|
| `CLOSE_IPC_HANDLE(orig_fd)` | Release pin for matching `orig_fd` |
| vmem fd closed (normal) | All remaining pins released automatically |
| Process crash | fd close triggers full sweep of `import_pins` |

> **Hard-pin (future):** For XE >= 6.19, enable the `#if 0` block in `vmem_dmabuf.c`
> (`dma_buf_pin()` → `xe_bo_pin_external()` → `ttm_bo_pin()`) for a stronger guarantee.

---

## Integration Test

```bash
insmod vmem.ko
cd test && ./vmem_gpu_test
```

**Expected output (single-machine, without astera_vfe.ko):**

```
[Phase 1]  L0 init                        PASS
[Phase 2]  Discover GPUs                  PASS  39:00.0 / 3f:00.0
[Phase 3]  Alloc 16 MiB VRAM (GPU0)       PASS
[Phase 4]  Export dma-buf fd              PASS  fd=8
[Phase 5]  Open /dev/vmemIntel + VERSION  PASS  v2.0.0
[Phase 6]  OPEN_IPC_HANDLE (GPU0)         PASS  1 entry  offset=0x5eb000000
[Phase 7]  Verify offset in GPU0 BAR2     PASS
[Phase 8]  GET_IPC_HANDLE (Method 1)      SKIP  -ENOSYS (astera_vfe.ko not loaded)
[Phase 8b] PA math self-check             PASS  0x22e800000000 + 0x5eb000000 = 0x22edeb000000
[Phase 9]  GPU0 direct readback           PASS  16 MiB = 0xAB, PFN math verified
[Phase 10] CLOSE_IPC_HANDLE + cleanup     done
=== Result: PASS ===
```

Phase 8 is skipped until `astera_vfe.ko` is present. The PA math is validated in Phase 8b
using the same formula the KMD would use with a real vFE endpoint.

---

## Design Notes

### Why PA synthesis moved into the KMD (v1.0 → v2.0)

In v1.0 the daemon computed `PA = vFE_bar_base + offset` using the Astera userspace SDK
and passed the result to `GET_IPC_FD`. This was clean when the SDK was available, but
introduced a daemon on the critical path for every new IPC handle.

In v2.0 (Method 1):
- The KMD calls an in-kernel `astera_lookup()` registered by `astera_vfe.ko`.
- The daemon becomes a thin transport — it forwards raw scatter offsets without any
  address computation.
- The kernel-side lookup is fast (spinlock-protected function pointer, no sleeping).
- `astera_vfe.ko` can be updated independently without modifying vmem KMD.

The registration stub pattern (`vmem_register_astera_lookup`) avoids a hard module
dependency: vmem.ko loads and functions for all ioctls except `GET_IPC_HANDLE` even
when `astera_vfe.ko` is absent.

### Dynamic scatter count

`OPEN_IPC_HANDLE` returns `-ENOSPC` with `count` set to the required value when the
caller's buffer is too small. The caller reallocates and retries. No hard kernel limit.

---

## Changelog

### v2.0.0

- **Architecture:** PA synthesis moved from userspace daemon into KMD (Method 1)
- **New `GET_IPC_HANDLE`:** takes `{node_id, gpu_id, BAR-relative offsets}`; KMD calls
  `vmem_astera_lookup()` → computes `base + offset` → builds pseudo dma-buf
- **New `vmem_astera.h` / `vmem_astera.c`:** function-pointer registration stub;
  exports `vmem_register_astera_lookup` / `vmem_unregister_astera_lookup` for
  `astera_vfe.ko` to use; returns `-ENOSYS` gracefully when driver is absent
- **Renamed ioctls** to match IPC lifecycle (`open/get/put/close`); v1.0 names kept
  as backward compat aliases at the same ioctl codes
- **`vmem_create_dmabuf_from_kpa()`:** internal helper (kernel-space PA array);
  decouples PA synthesis from dma-buf construction
- **`vmem_open_ipc_handle()` / `vmem_close_ipc_handle()`:** renamed from
  `vmem_parse_dmabuf` / `vmem_release_pin`
- Daemon no longer required for PA computation; forwards raw scatter data only

### v1.0.0

- `GET_IPC_FD`: removed `consumer_bus/device/function`; PAs supplied by daemon
- `GET_PFN_OFFSET_LIST`: BAR-relative offsets; userspace pointer; dynamic count with
  `-ENOSPC` retry
- `vmem_buf.entries[]` dynamically allocated
- Per-fd persistent soft-pin (`vmem_import_pin`); `PUT_IPC_FD` releases pin
- Added: `VMEM_IOCTL_VERSION`, `VMEM_IOCTL_IDENTIFY_DMABUF`
- Added: `begin/end_cpu_access`, `dma_coerce_mask_and_coherent`,
  `MODULE_IMPORT_NS("DMA_BUF")`, `_IOWR` encoding

### v0.1.0

Initial: global `REGISTER_EP` table, 65 KB embedded struct per ioctl, fixed entry
limit, `PUT_IPC_FD` no-op, consumer BAR resolved in kernel.
