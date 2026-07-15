# vMem KMD — PCIe Cross-Node GPU dma-buf Translator

**Version:** 1.0.0 | **License:** GPL-2.0 | **Device:** `/dev/vmemIntel`

## Overview

vMem is a Linux kernel module that enables cross-node GPU memory sharing over a PCIe Gen5
switch. It provides a minimal two-function kernel interface:

1. **Producer side** — attach to a GPU dma-buf and extract BAR-relative scatter offsets
2. **Consumer side** — wrap a pre-computed physical address list into a synthetic dma-buf

All address translation (`BAR-relative offset -> absolute physical address`) is done in
**userspace by a daemon**, using the Astera vFE driver to obtain the correct vFE BAR base.
The kernel driver never resolves consumer-side BARs.

```
Producer Node                                        Consumer Node
─────────────────                                    ─────────────────
GPU0 VRAM                                            GPU1
zeMemGetIpcHandle()         PCIe Gen5 Switch         zeMemOpenIpcHandle()
dma-buf fd                  Astera PEX890xx          remote_ptr (P2P DMA)
      |                     (BAR-to-BAR routing)             ^
      v                                                       |
GET_PFN_OFFSET_LIST                                  GET_IPC_FD(PA list)
  -> BAR-relative offsets                                     |
           |                                                  |
           |         Userspace Daemon                         |
           +-------> PA = vFE_bar_base + offset  ------------+
                     (Astera vFE driver API)
```

### Layer Responsibilities

| Layer | Role |
|-------|------|
| **vmem KMD** | Extract scatter offsets from GPU dma-buf; wrap PA list into fake dma-buf |
| **Daemon** | Query Astera vFE driver for BAR base; compute `PA = vFE_bar_base + offset` |
| **Astera vFE driver** | Enumerate vFE endpoints; expose BAR window routing to remote VRAM |

### Status

| Mode | Result | Notes |
|------|--------|-------|
| Single-machine (2 GPUs, 1 host) | **PASS** | All ioctls verified, PA math correct |
| Real cross-node P2P | Hardware-dependent | Requires Astera switch BAR-to-BAR routing |

---

## Hardware (Verified)

```
GPU0 (producer): BDF 39:00.0  Intel Arc e211  BAR2 @ 0x22f000000000  32 GiB
GPU1 (consumer): BDF 3f:00.0  Intel Arc e211  BAR2 @ 0x22e800000000  32 GiB
PCIe Switch:     BDF 09:00.0  Broadcom/LSI PEX890xx PCIe Gen5
Kernel:          6.14.0-36-generic
GPU driver:      Intel XE (xe.ko, in-kernel)
Level Zero:      libze_loader.so.1
```

> **IOMMU:** Set `intel_iommu=pt` (passthrough) so DMA addresses equal physical addresses.
> Without this, cross-node PFN transport is invalid.

---

## Repository Layout

```
driver.gpu.vmem/
├── include/
│   └── vmem_ioctl.h        UAPI — include this in both KMD and UMD
├── vmem_dmabuf.h           Internal types (vmem_buf, vmem_import_pin, vmem_file_priv)
├── vmem_drv.c              Module init, /dev/vmemIntel misc device, ioctl dispatch
├── vmem_dmabuf.c           Producer parse, consumer create, identify, pin lifecycle
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
make              # builds vmem.ko
make install      # insmod vmem.ko  ->  /dev/vmem
make uninstall    # rmmod vmem

# Integration test
cd test
gcc -O2 -Wall -g -I.. -o vmem_gpu_test vmem_gpu_test.c -lze_loader -ldl
```

---

## IOCTL API

Include `include/vmem_ioctl.h`. Magic number: `'V'` (0x56).

### Commands

| Command | Code | Dir | Description |
|---------|------|-----|-------------|
| `VMEM_IOCTL_VERSION` | 0x00 | `_IOR` | Query driver version |
| `VMEM_IOCTL_GET_PFN_OFFSET_LIST` | 0x01 | `_IOWR` | Producer: attach GPU dma-buf, return BAR-relative scatter offsets |
| `VMEM_IOCTL_GET_IPC_FD` | 0x02 | `_IOWR` | Consumer: wrap PA list into synthetic dma-buf (no BDF needed) |
| `VMEM_IOCTL_CLOSE_IPC_FD` | 0x03 | `_IOW` | Consumer: destroy the synthetic dma-buf fd |
| `VMEM_IOCTL_PUT_IPC_FD` | 0x04 | `_IOW` | Producer: release persistent GPU dma-buf attachment |
| `VMEM_IOCTL_IDENTIFY_DMABUF` | 0x06 | `_IOWR` | Probe which GPU's VRAM backs a dma-buf |

### Core Type

```c
/* Scatter entry.
 *   GET_PFN_OFFSET_LIST (producer): offset = BAR2-relative byte offset from GPU VRAM base
 *   GET_IPC_FD          (consumer): offset = absolute physical address
 *                                   (computed by daemon: vFE_bar_base + BAR-relative offset)
 */
struct vmem_pfn_entry {
    __u64 offset;
    __u32 size;     /* page-aligned byte length */
    __u32 pad;
};
```

### Argument Structures

```c
/* GET_PFN_OFFSET_LIST — producer */
struct vmem_ioctl_get_pfn_arg {
    __s32  fd;           /* in:  GPU dma-buf fd */
    __u8   bus;          /* in:  GPU PCIe bus */
    __u8   device;       /* in:  GPU PCIe device */
    __u8   function;     /* in:  GPU PCIe function */
    __u8   pad2;
    __u32  count;        /* in/out: capacity / actual entry count; -ENOSPC if too small */
    __u32  pad;
    __u64  entries_ptr;  /* in:  userspace ptr to vmem_pfn_entry[] output buffer */
};

/* GET_IPC_FD — consumer (no BDF: daemon already computed PAs) */
struct vmem_ioctl_get_ipc_fd_arg {
    __u32  count;        /* in:  number of PA entries */
    __u32  pad;
    __u64  entries_ptr;  /* in:  userspace ptr to vmem_pfn_entry[]
                                 entries[i].offset = absolute physical address
                                 (daemon: vFE_bar_base + scatter_offset) */
    __s32  fd;           /* out: synthetic dma-buf fd */
    __u32  pad2;
};
```

---

## Usage

### 1. Producer

```c
int vmem_fd = open("/dev/vmemIntel", O_RDWR);

/* Export GPU buffer */
ze_ipc_mem_handle_t ipc_h;
zeMemGetIpcHandle(ctx, gpu_ptr, &ipc_h);
int dma_fd;
memcpy(&dma_fd, ipc_h.data, sizeof(int));

/* Extract BAR-relative scatter offsets (ENOSPC retry pattern) */
uint32_t count = VMEM_MAX_PFN_ENTRIES;
struct vmem_pfn_entry *entries = calloc(count, sizeof(*entries));

retry:;
struct vmem_ioctl_get_pfn_arg arg = {
    .fd = dma_fd, .bus = BUS, .device = DEV, .function = FN,
    .count = count, .entries_ptr = (uint64_t)(uintptr_t)entries,
};
if (ioctl(vmem_fd, VMEM_IOCTL_GET_PFN_OFFSET_LIST, &arg) < 0 && errno == ENOSPC) {
    count = arg.count;
    entries = realloc(entries, count * sizeof(*entries));
    goto retry;
}
/* arg.count = actual BAR-relative entries in entries[] */

/* Send entries[] + arg.count to daemon via IPC */
send_to_daemon(entries, arg.count);

/* Release pin when done */
struct vmem_ioctl_put_ipc_fd_arg put = { .fd = dma_fd };
ioctl(vmem_fd, VMEM_IOCTL_PUT_IPC_FD, &put);
zeMemPutIpcHandle(ctx, ipc_h);
free(entries);
close(vmem_fd);
```

### 2. Daemon — PA synthesis

```c
/* Receive BAR-relative entries from producer */
struct vmem_pfn_entry *bar_entries;
uint32_t count;
recv_from_producer(&bar_entries, &count);

/* Get vFE BAR base from Astera vFE driver / SDK */
uint64_t vfe_bar_base = astera_get_vfe_bar_base(remote_node_id, remote_gpu_id);

/* Compute absolute PAs */
struct vmem_pfn_entry *pa_entries = calloc(count, sizeof(*pa_entries));
for (uint32_t i = 0; i < count; i++) {
    pa_entries[i].offset = vfe_bar_base + bar_entries[i].offset;  /* absolute PA */
    pa_entries[i].size   = bar_entries[i].size;
}

/* Forward PA list to consumer */
send_to_consumer(pa_entries, count);
free(pa_entries);
```

### 3. Consumer

```c
int vmem_fd = open("/dev/vmemIntel", O_RDWR);

/* Receive PA entries from daemon */
struct vmem_pfn_entry *pa_entries;
uint32_t count;
recv_from_daemon(&pa_entries, &count);

/* Build synthetic dma-buf — driver maps PAs directly, no BDF */
struct vmem_ioctl_get_ipc_fd_arg arg = {
    .count = count,
    .entries_ptr = (uint64_t)(uintptr_t)pa_entries,
};
ioctl(vmem_fd, VMEM_IOCTL_GET_IPC_FD, &arg);
int fake_fd = arg.fd;

/* Import into consumer GPU */
ze_ipc_mem_handle_t fake_ipc = {};
memcpy(fake_ipc.data, &fake_fd, sizeof(int));
void *remote_ptr;
zeMemOpenIpcHandle(ctx, consumer_gpu, fake_ipc,
                   ZE_IPC_MEMORY_FLAG_BIAS_UNCACHED, &remote_ptr);
/* P2P DMA through remote_ptr ... */

/* Cleanup */
zeMemCloseIpcHandle(ctx, remote_ptr);
struct vmem_ioctl_close_ipc_fd_arg close_arg = { .fd = fake_fd };
ioctl(vmem_fd, VMEM_IOCTL_CLOSE_IPC_FD, &close_arg);
free(pa_entries);
close(vmem_fd);
```

---

## Persistent Attachment (Soft-Pin)

After `GET_PFN_OFFSET_LIST` returns, the driver stores `{dmabuf, attach, sgt, pdev}` in a
per-fd `vmem_import_pin` list. This keeps the XE driver's TTM from evicting the VRAM BO
while the daemon/consumer holds the PFN list.

| Event | Action |
|-------|--------|
| `PUT_IPC_FD` called | Release pin for the matching `orig_fd` |
| vmem fd closed | All remaining pins released automatically |
| Process crash | fd close triggers full cleanup |

> **Hard-pin (future):** For XE >= 6.19, enable the `#if 0` block in `vmem_dmabuf.c`
> (`dma_buf_pin()` -> `xe_bo_pin_external()` -> `ttm_bo_pin()`) for a stronger guarantee.

---

## Integration Test

```bash
insmod vmem.ko
cd test && ./vmem_gpu_test
```

**Expected output (single-machine):**
```
[Phase 1]  L0 init                        PASS
[Phase 2]  Discover GPUs                  PASS  39:00.0 / 3f:00.0
[Phase 3]  Alloc 16 MiB VRAM (GPU0)       PASS
[Phase 4]  Export dma-buf fd              PASS  fd=8
[Phase 5]  Open /dev/vmemIntel + VERSION       PASS  v1.0.0
[Phase 6]  (daemon: PA = vFE_bar_base + scatter_offset)
[Phase 7]  GET_PFN_OFFSET_LIST            PASS  1 entry  offset=0x5eb000000
[Phase 8]  Verify offset in GPU0 BAR2     PASS
[Phase 9]  PA synthesis + GET_IPC_FD      PASS
              pa[0]: 0x22e800000000 + 0x5eb000000 = 0x22edeb000000
[Phase 10] zeMemOpenIpcHandle (GPU1)      PASS
[Phase 11] P2P data verify                WARN  mismatch expected (no switch routing)
[Phase 11b] GPU0 direct readback          PASS  16 MiB = 0xAB, PFN math verified
[Phase 12] CLOSE_IPC_FD + cleanup         done
=== Result: PASS ===
```

Phase 9 shows the daemon's PA synthesis step in the test output.

Phase 11 WARN is **expected** in single-machine mode: GPU1 BAR2 maps GPU1's own LMEM.
A real Astera switch routes GPU1-BAR2 accesses to GPU0 LMEM. The driver correctness
is proven by Phases 7–9 (offset extraction + PA wrapping) and Phase 11b (PFN math).

---

## Design Notes

### Why PA synthesis is in the daemon, not the kernel

In a real Astera PCIe switch topology, the consumer host sees a virtual Fabric Endpoint
(vFE) device whose BAR2 window is routed to remote GPU VRAM by the switch. The vFE BAR
base address is assigned by the switch and exposed via the Astera SDK — it may differ from
what the OS PCI enumerator reports (e.g. after IOMMU remapping or switch address translation).

Resolving this in the kernel via `pci_resource_start()` would be fragile. Having the daemon
query the Astera SDK and pass the resolved PA directly to `GET_IPC_FD` decouples the kernel
driver from switch topology, making vmem KMD portable across different switch configurations
without any driver changes.

### Dynamic scatter count

`GET_PFN_OFFSET_LIST` returns `-ENOSPC` with `count` set to the required value when the
caller's buffer is too small. The caller reallocates and retries. No hard kernel limit on
the number of scatter entries per buffer.

---

## Changelog

### v1.0.0

- **GET_IPC_FD**: removed `consumer_bus/device/function` and `pci_resource_start()` — PAs now
  supplied by daemon, no BDF in kernel
- **GET_PFN_OFFSET_LIST**: always returns BAR-relative offsets; removed `ABS_PA` flag;
  userspace pointer + dynamic count with `-ENOSPC` retry
- **vmem_buf**: `entries[]` dynamically allocated; removed `ep_bar2_base`, `abs_pa`
- Removed: `REGISTER_EP`, `VMEM_GET_PFN_FLAG_ABS_PA`, `VMEM_IPC_FLAG_ABS_PA`
- Added: per-fd persistent soft-pin (`vmem_import_pin`), `PUT_IPC_FD` correctly releases pin
- Added: `VMEM_IOCTL_VERSION`, `VMEM_IOCTL_IDENTIFY_DMABUF`
- Added: `begin/end_cpu_access` callbacks, `dma_coerce_mask_and_coherent`,
  `MODULE_IMPORT_NS("DMA_BUF")`, `_IOWR` ioctl encoding

### v0.1.0

Initial: global `REGISTER_EP` table, 65 KB embedded struct per ioctl, fixed entry limit,
`PUT_IPC_FD` no-op, consumer BAR resolved in kernel.
