# vMem KMD — PCIe Cross-Node GPU dma-buf Translator

**Version:** 1.0.0 | **License:** GPL-2.0 | **Device:** `/dev/vmem`

## Overview

vMem is a Linux kernel module that enables cross-node GPU memory sharing over a PCIe Gen5 switch.
It acts as a **dma-buf translator** between two GPU nodes:

- **Producer node** exports GPU VRAM as a dma-buf fd (via `zeMemGetIpcHandle`)
- vMem extracts the scatter-gather BAR offsets from the dma-buf and keeps the attachment alive (soft-pin)
- **Consumer node** reconstructs a synthetic dma-buf mapping the remote VRAM through the switch
- Consumer GPU imports the synthetic dma-buf (via `zeMemOpenIpcHandle`) and performs P2P DMA

```
Producer Node                                    Consumer Node
─────────────────                                ─────────────────
GPU0 VRAM                 PCIe Gen5 Switch       GPU1
  zeMemGetIpcHandle()     Astera PEX890xx          zeMemOpenIpcHandle()
  dma-buf fd              (BAR2->BAR2 routing)     remote_ptr
        |                                               ^
        v                                               |
  GET_PFN_OFFSET_LIST ---[BAR offsets / IPC]---> GET_IPC_FD
```

### Current Status

| Mode | Status | Notes |
|------|--------|-------|
| Single-machine (two GPUs, one host) | Verified PASS | All ioctls functional, PFN math correct |
| Real cross-node P2P | Hardware-dependent | Requires PCIe switch BAR-to-BAR routing |

---

## Hardware Setup (Verified)

```
GPU0 (producer): BDF 39:00.0  Intel Arc e211  BAR2 @ 0x22f000000000  32 GiB
GPU1 (consumer): BDF 3f:00.0  Intel Arc e211  BAR2 @ 0x22e800000000  32 GiB
PCIe Switch:     BDF 09:00.0  Broadcom/LSI PEX890xx PCIe Gen5
Kernel:          6.14.0-36-generic (Ubuntu 22.04/24.04)
GPU driver:      Intel XE (in-kernel, xe.ko)
Level Zero:      libze_loader.so.1
```

> **IOMMU requirement:** Must be in passthrough mode (`intel_iommu=pt` in kernel cmdline)
> so DMA addresses equal physical addresses. Without this, cross-node PFN transport is invalid.

---

## Repository Layout

```
driver.gpu.vmem/
├── include/
│   └── vmem_ioctl.h        UAPI — shared between KMD and UMD
├── vmem_dmabuf.h           Internal driver types (vmem_buf, vmem_import_pin, vmem_file_priv)
├── vmem_drv.c              Module init, /dev/vmem misc device, ioctl dispatch
├── vmem_dmabuf.c           dma-buf parse/create/identify, persistent pin lifecycle
├── Makefile
├── README.md               This file
└── test/
    ├── vmem_gpu_test.c     Full GPU integration test (requires Level Zero + 2x Arc e211)
    └── vmem_test.c         Basic ioctl smoke test (no GPU needed)
```

---

## Build

### Prerequisites

```bash
# Kernel headers
apt install linux-headers-$(uname -r)

# Level Zero SDK (GPU test only)
apt install libze-dev libze-intel-gpu-dev level-zero
```

### Kernel Module

```bash
cd driver.gpu.vmem
make              # builds vmem.ko
make install      # insmod vmem.ko  (creates /dev/vmem)
make uninstall    # rmmod vmem
make info         # modinfo vmem.ko
```

### Integration Test

```bash
cd test
gcc -O2 -Wall -g -I.. -o vmem_gpu_test vmem_gpu_test.c -lze_loader -ldl
```

---

## IOCTL API

Include `include/vmem_ioctl.h` in userspace code. Magic number: `'V'` (0x56).

### Command Table

| Command | Code | Dir | Description |
|---------|------|-----|-------------|
| `VMEM_IOCTL_VERSION` | 0x00 | `_IOR` | Query driver version (major.minor.patch) |
| `VMEM_IOCTL_GET_PFN_OFFSET_LIST` | 0x01 | `_IOWR` | Producer: attach GPU dma-buf, extract BAR-relative scatter offsets |
| `VMEM_IOCTL_GET_IPC_FD` | 0x02 | `_IOWR` | Consumer: build synthetic dma-buf from offset list |
| `VMEM_IOCTL_CLOSE_IPC_FD` | 0x03 | `_IOW` | Consumer: destroy the synthetic dma-buf fd |
| `VMEM_IOCTL_PUT_IPC_FD` | 0x04 | `_IOW` | Producer: release the persistent GPU dma-buf attachment |
| `VMEM_IOCTL_IDENTIFY_DMABUF` | 0x06 | `_IOWR` | Probe which GPU's VRAM backs an arbitrary dma-buf |

### Core Data Type

```c
/* One scatter-gather entry transferred from producer to consumer */
struct vmem_pfn_entry {
    __u64 offset;   /* BAR2-relative byte offset (absolute PA if ABS_PA flag) */
    __u32 size;     /* byte length of this scatter chunk (page-aligned)        */
    __u32 pad;
};
```

### Flags

| Flag | Applies to | Meaning |
|------|-----------|---------|
| `VMEM_GET_PFN_FLAG_ABS_PA` | `GET_PFN_OFFSET_LIST` | `offset` = absolute physical address instead of BAR-relative |
| `VMEM_IPC_FLAG_ABS_PA` | `GET_IPC_FD` | entries contain absolute PAs; skip BAR2 base addition |

---

## Usage

### Producer (local GPU owner)

```c
int vmem_fd = open("/dev/vmem", O_RDWR);

/* Step 1: export GPU buffer via Level Zero */
ze_ipc_mem_handle_t ipc_h;
zeMemGetIpcHandle(ctx, gpu_ptr, &ipc_h);
int dma_fd;
memcpy(&dma_fd, ipc_h.data, sizeof(int));

/* Step 2: allocate entry buffer */
uint32_t count = VMEM_MAX_PFN_ENTRIES;  /* start with recommended max */
struct vmem_pfn_entry *entries = calloc(count, sizeof(*entries));

/* Step 3: extract BAR-relative scatter offsets (with ENOSPC retry) */
retry:;
struct vmem_ioctl_get_pfn_arg pfn_arg = {
    .fd          = dma_fd,
    .bus         = GPU0_BUS, .device = GPU0_DEV, .function = GPU0_FN,
    .flags       = 0,            /* VMEM_GET_PFN_FLAG_ABS_PA for fabric topologies */
    .count       = count,        /* IN: buffer capacity */
    .entries_ptr = (uint64_t)(uintptr_t)entries,
};
int rc = ioctl(vmem_fd, VMEM_IOCTL_GET_PFN_OFFSET_LIST, &pfn_arg);
if (rc < 0 && errno == ENOSPC) {
    count = pfn_arg.count;       /* OUT: required count */
    entries = realloc(entries, count * sizeof(*entries));
    goto retry;
}
/* pfn_arg.count now = actual number of entries written to entries[] */

/* Step 4: send entries[] and pfn_arg.count to consumer via IPC */
send_to_consumer(entries, pfn_arg.count);

/* Step 5: release the persistent attachment when done */
struct vmem_ioctl_put_ipc_fd_arg put_arg = { .fd = dma_fd };
ioctl(vmem_fd, VMEM_IOCTL_PUT_IPC_FD, &put_arg);
zeMemPutIpcHandle(ctx, ipc_h);
free(entries);
close(vmem_fd);
```

### Consumer (remote GPU importer)

```c
int vmem_fd = open("/dev/vmem", O_RDWR);

/* Receive entries[] + count from producer */
struct vmem_pfn_entry *entries;
uint32_t count;
recv_from_producer(&entries, &count);

/* Build synthetic dma-buf (driver resolves GPU1 BAR2 from BDF automatically) */
struct vmem_ioctl_get_ipc_fd_arg ipc_arg = {
    .consumer_bus = GPU1_BUS, .consumer_device = GPU1_DEV, .consumer_function = GPU1_FN,
    .flags       = 0,         /* VMEM_IPC_FLAG_ABS_PA if producer used ABS_PA */
    .count       = count,
    .entries_ptr = (uint64_t)(uintptr_t)entries,
};
ioctl(vmem_fd, VMEM_IOCTL_GET_IPC_FD, &ipc_arg);
int fake_fd = ipc_arg.fd;

/* Import into GPU1 for P2P access */
ze_ipc_mem_handle_t fake_ipc = {};
memcpy(fake_ipc.data, &fake_fd, sizeof(int));
void *remote_ptr;
zeMemOpenIpcHandle(ctx, gpu1, fake_ipc, ZE_IPC_MEMORY_FLAG_BIAS_UNCACHED, &remote_ptr);

/* ... GPU1 performs DMA read/write through remote_ptr ... */

/* Cleanup */
zeMemCloseIpcHandle(ctx, remote_ptr);
struct vmem_ioctl_close_ipc_fd_arg close_arg = { .fd = fake_fd };
ioctl(vmem_fd, VMEM_IOCTL_CLOSE_IPC_FD, &close_arg);
free(entries);
close(vmem_fd);
```

---

## Persistent Attachment (Soft-Pin)

After `GET_PFN_OFFSET_LIST` returns, the driver keeps the `dma_buf_attachment` + `sg_table` alive
in a per-fd pin list (`vmem_import_pin`). This prevents the Intel XE driver's TTM memory manager
from migrating or evicting the VRAM buffer while the consumer holds the PFN list.

| Event | Action |
|-------|--------|
| `PUT_IPC_FD` called | Release pin for that specific `orig_fd` |
| vmem fd closed | All remaining pins released (`vmem_import_pins_cleanup`) |
| Process crash | fd close triggers cleanup automatically |

> **Hard-pin (future):** On XE kernel >= 6.19, `dma_buf_pin()` → `xe_bo_pin_external()` →
> `ttm_bo_pin()` provides a stronger eviction guarantee. The code path is prepared with a
> `#if 0` placeholder in `vmem_dmabuf.c`.

---

## Running the Integration Test

```bash
# Load the module
insmod vmem.ko

# Run test (requires Level Zero, 2x Intel Arc e211)
cd test
./vmem_gpu_test
```

### Expected Result

```
=== vmem driver GPU integration test ===
[Phase 1]  L0 init                        PASS
[Phase 2]  Discover GPUs                  PASS  gpu0=39:00.0  gpu1=3f:00.0
[Phase 3]  Allocate 16 MiB VRAM (GPU0)    PASS  ptr=0xffff...
[Phase 4]  Export dma-buf fd              PASS  fd=8  size=16777216
[Phase 5]  Open /dev/vmem + VERSION       PASS  v1.0.0
[Phase 6]  (REGISTER_EP removed)          --    consumer BDF passed per-call
[Phase 7]  GET_PFN_OFFSET_LIST            PASS  1 entry  offset=0x5eb000000
[Phase 8]  Verify PFN in GPU0 BAR2        PASS  16 MiB mapped
[Phase 9]  GET_IPC_FD (consumer=GPU1)     PASS  fake_fd=10
[Phase 10] zeMemOpenIpcHandle (GPU1)      PASS  remote_ptr=0xffff...
[Phase 11] P2P data verification          WARN  mismatch (expected, no switch routing)
[Phase 11b] GPU0 direct readback          PASS  16 MiB = 0xAB, PFN math verified
[Phase 12] CLOSE_IPC_FD + cleanup         done
=== Result: PASS ===
```

**Phase 11 WARN is expected** in single-machine mode: GPU1's BAR2 window maps GPU1's own LMEM,
not GPU0's. A real PCIe switch provides BAR-to-BAR routing so accesses through GPU1-BAR2 are
forwarded to GPU0-LMEM. Phases 7–9 and 11b confirm the driver's PFN extraction and synthetic
dma-buf construction are correct.

---

## Design Decisions

### Per-Call Consumer BDF (no REGISTER_EP)

The consumer GPU's BAR2 base is resolved each time via `pci_resource_start(pdev, 2)` using the
BDF fields in `GET_IPC_FD`. There is no global endpoint registry — multiple consumers with
different GPUs coexist without conflict, and no daemon/setup step is required.

### Dynamic Scatter Count with ENOSPC Retry

`GET_PFN_OFFSET_LIST` uses a userspace pointer + `count` field (IN=capacity, OUT=actual).
If the caller's buffer is too small, the ioctl returns `-ENOSPC` with `count` set to the
required size. The caller reallocates and retries. There is no hard entry limit in the kernel.

### ABS_PA Flag for Fabric Topologies

For topologies where remote GPU memory is directly addressable (e.g. CXL or fabric-attached
memory), set `VMEM_GET_PFN_FLAG_ABS_PA` on the producer. The `offset` field then carries the
absolute physical address. The consumer passes `VMEM_IPC_FLAG_ABS_PA` to skip the BAR2 addition.

---

## Changelog

### v1.0.0

- `GET_IPC_FD`: `consumer_bus/device/function` replaces global `REGISTER_EP` table
- `GET_PFN_OFFSET_LIST`: userspace pointer + dynamic count, `-ENOSPC` retry, `ABS_PA` flag
- `vmem_buf.entries`: dynamically allocated (no fixed array limit)
- Persistent soft-pin (`vmem_import_pin`) to prevent TTM eviction
- `PUT_IPC_FD`: now correctly releases the pin (was a no-op in v0.1)
- `VMEM_IOCTL_VERSION` and `VMEM_IOCTL_IDENTIFY_DMABUF` added
- `begin_cpu_access` / `end_cpu_access` callbacks added to dma-buf ops
- `dma_coerce_mask_and_coherent` set on misc device
- `MODULE_IMPORT_NS("DMA_BUF")` added
- `_IO` encoding upgraded to `_IOWR` (proper struct size in ioctl number)

### v0.1.0

- Initial implementation: `REGISTER_EP` global table, 65 KB embedded struct per ioctl,
  fixed `VMEM_MAX_PFN_ENTRIES` limit, `PUT_IPC_FD` no-op
