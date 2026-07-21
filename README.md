# vMem KMD — PCIe Cross-Node GPU dma-buf Pass-Through

**Version:** 4.0.0 | **License:** GPL-2.0 | **Device:** `/dev/vmemIntel`

## Overview

vMem is a Linux kernel module that enables cross-node GPU memory sharing over a PCIe Gen5
switch (Astera PEX890xx). It exposes a `/dev/vmemIntel` misc device. No modifications to
the Intel xe GPU driver are required.

**Method 2 (this build): pass-through. KMD and UMD perform NO address translation.**
Address translation is done in userspace by the **IMEMLINK daemon (Layer A)** via the
Astera COSMOS SDK. KMD merely walks the sg_table on the origin side and builds the MMIO
pseudo dma-buf from pre-translated PAs on the peer side.

- **Origin node**: attach GPU dma-buf (xe-exported), walk sg_table, return **absolute
  physical addresses** per scatter chunk. Daemon subtracts GPU BAR2 base and stores
  PA offsets in BlobStore.
- **Peer node**: daemon queries Astera COSMOS SDK for the vFE endpoint, computes
  TARGET PAs, passes them to UMD. KMD builds MMIO pseudo dma-buf **directly from
  the provided absolute PAs** — no `astera_lookup` in kernel.

---

## Architecture

```
Origin Node                                    Peer Node
─────────────────────────────────────────      ─────────────────────────────────────────
GPU App (oneCCL)                               GPU App (oneCCL)
  │ zeMemGetIpcHandle(base_va) → xe fd           │ zeMemOpenIpcHandle(dummy_fd) → VA
  │ pass xe fd via UDS + SCM_RIGHTS              ↑ dummy_fd via SCM_RIGHTS
  ↓                                              │
IMEMLINK daemon (Layer A)                      IMEMLINK daemon (Layer A)
  │ vmem_open_dmabuf(ctx, xe_fd, bdf)            │ blob_fetch → pa_offset[]
  │   → VMEM_IOCTL_OPEN_DMABUF                   │ cosmos_query_vfe(node_id, gpu_id)
  │ abs_pa[] (from KMD)                          │   → vfe_base
  │ pa_offset[i] = abs_pa[i] - bar2_base         │ TARGET_PA[i] = vfe_base + pa_offset[i]
  │ BlobStore → uuid                             │ vmem_get_dmabuf(ctx, TARGET_PA[])
  │                                              │   → VMEM_IOCTL_GET_DMABUF
  ↓                                              │ dummy_fd
vMem KMD (/dev/vmemIntel)                      vMem KMD (/dev/vmemIntel)
  │ dma_buf_dynamic_attach()                     │ build MMIO pseudo dma-buf
  │ dma_buf_map_attachment() → sg_table          │ from TARGET_PA[] directly
  │ entries[i].addr = sg_dma_address(sg)         │ (NO astera_lookup)
  │   (abs PA; IOMMU-off → PA == DMA addr)       │ dma_buf_export() → dummy_fd
  │ soft-pin (attach + sgt kept alive)           │ vmem_buffer_obj tracked (kref)
  │ debugfs: /imported                           │ debugfs: /exported
  ↓                                              ↓
                     Astera PCIe Switch (PEX890xx)
                    P2P DMA: peer GPU → origin VRAM
                    (never through CPU or daemon)
```

### Module Decomposition

| File | Responsibility |
|------|----------------|
| `vmem_drv.c` | Module init/exit, misc device, `open`/`release`, ioctl dispatch. Sets 64-bit DMA mask. |
| `vmem_dmabuf.c` | **Origin path**: P2P attach → map → abs PA extraction (sg_dma_address). **Peer path**: copy pre-translated TARGET PAs from UMD, build MMIO pseudo dma-buf. Debugfs hooks on open/get/close/put. |
| `vmem_buffer.c` | `vmem_buffer_obj` lifecycle: kref init/get/put → release frees segs + obj. Per-fd `buffers` list for backstop cleanup. |
| `vmem_debugfs.c` | Global `imported`/`exported` registry (two lists + one mutex). Read-only seq_file under `/sys/kernel/debug/vmemIntel/`. |

---

## Repository Layout

```
driver.gpu.vmem/
├── include/
│   └── vmem_ioctl.h        UAPI — include in UMD (libvmem)
├── vmem_buffer.h/c         vmem_buffer_obj kref lifecycle
├── vmem_debugfs.h/c        Global registry + debugfs
├── vmem_dmabuf.h/c         Export + import dma-buf operations (pass-through)
├── vmem_drv.c              Device registration, ioctl dispatch
├── Makefile
├── README.md
└── test/
    └── vmem_test.c         Ioctl smoke test (VERSION, OPEN/GET/PUT/CLOSE)
```

---

## Build

```bash
# Prerequisites
apt install linux-headers-$(uname -r)

# Build
make              # -> vmem.ko
make install      # insmod vmem.ko  ->  /dev/vmemIntel
make uninstall    # rmmod vmem
make info         # modinfo vmem.ko
```

> **IOMMU:** Set `intel_iommu=pt` (passthrough) so `sg_dma_address()` equals the CPU
> physical address.

---

## IOCTL API

Include `include/vmem_ioctl.h`. Magic: `'V'` (0x56). Version: **4.0.0**.

### Command Table

| Command | Code | Direction | Description |
|---------|------|-----------|-------------|
| `VMEM_IOCTL_VERSION` | 0x00 | `_IOR` | Query driver version |
| `VMEM_IOCTL_OPEN_DMABUF` | 0x01 | `_IOWR` | Origin: P2P attach, walk sg_table, return **absolute PAs** in `entries[i].addr`. Daemon computes `pa_offset = abs_pa - bar2_base`. |
| `VMEM_IOCTL_GET_DMABUF` | 0x02 | `_IOWR` | Peer: build MMIO pseudo dma-buf from **pre-translated TARGET PAs** in `entries[i].addr`. No `node_id`/`gpu_id`. No astera_lookup. |
| `VMEM_IOCTL_PUT_DMABUF` | 0x03 | `_IOW` | Peer: destroy pseudo dma-buf fd |
| `VMEM_IOCTL_CLOSE_DMABUF` | 0x04 | `_IOW` | Origin: release soft-pin (by opened fd) |

> Every object is identified by a **dma-buf fd** — no opaque handles.

### Two-step PFN Retrieval (`OPEN_DMABUF`)

```
1st call: count = 0
  -> KMD returns -ENOSPC, count = required N
Allocate: entries = malloc(N * sizeof(vmem_pfn_entry))
2nd call: count = N, entries_ptr = entries
  -> KMD fills entries[i].{addr = abs_pa, size}
Daemon: pa_offset[i] = entries[i].addr - gpu_bar2_base
```

### Key Structures

```c
/* One scatter chunk descriptor */
struct vmem_pfn_entry {
    __u64 addr;  /* OPEN: abs PA (sg_dma_address, IOMMU-off)
                  * GET:  TARGET PA (daemon-translated via COSMOS SDK) */
    __u32 size;  /* page-aligned byte length */
    __u32 pad;
};

/* OPEN_DMABUF */
struct vmem_ioctl_open_dmabuf_arg {
    __s32  fd;                      /* in:  GPU xe dma-buf fd */
    __u8   bus, device, function;   /* in:  GPU PCIe BDF */
    __u8   pad2;
    __u32  count;                   /* in/out: capacity / actual count */
    __u32  page_size;               /* out: PAGE_SIZE */
    __u64  total_size;              /* out: total bytes across all chunks */
    __u64  entries_ptr;             /* in:  vmem_pfn_entry[] output buffer */
};

/* GET_DMABUF (Method 2: pass-through) */
struct vmem_ioctl_get_dmabuf_arg {
    __u32  count;        /* in:  number of entries */
    __u32  pad;
    __u64  entries_ptr;  /* in:  vmem_pfn_entry[] (TARGET PAs from daemon) */
    __s32  fd;           /* out: pseudo dma-buf fd */
    __u32  pad2;
};
```

---

## Usage Example (Peer Side — called by daemon Layer A)

```c
int vmem_fd = open("/dev/vmemIntel", O_RDWR);

/* TARGET PAs computed by daemon via COSMOS SDK:
 *   cosmos_query_vfe(node_id, gpu_id) -> {vfe_dbdf, bar_id, slot}
 *   vfe_base = pci_resource_start(vfe_dbdf, bar_id) + slot * 32 GiB
 *   TARGET_PA[i] = vfe_base + pa_offset[i]
 */
struct vmem_pfn_entry entries[N];
for (int i = 0; i < N; i++) {
    entries[i].addr = target_pa[i];   /* pre-translated by daemon */
    entries[i].size = chunk_size[i];
}

struct vmem_ioctl_get_dmabuf_arg arg = {
    .count       = N,
    .entries_ptr = (uint64_t)(uintptr_t)entries,
};
if (ioctl(vmem_fd, VMEM_IOCTL_GET_DMABUF, &arg) < 0) {
    perror("GET_DMABUF"); goto out;
}
int dummy_fd = arg.fd;

/* Pass dummy_fd to libimemlink via SCM_RIGHTS;
 * libimemlink calls zeMemOpenIpcHandle(dummy_fd) -> imported_base_va */

/* Teardown */
struct vmem_ioctl_put_dmabuf_arg put = { .fd = dummy_fd };
ioctl(vmem_fd, VMEM_IOCTL_PUT_DMABUF, &put);
out:
close(vmem_fd);
```

---

## Lifetime & Cleanup

```
Peer:   VMEM_IOCTL_PUT_DMABUF(fd)    -> remove vmem_buffer_obj, debugfs del,
                                         close_fd(fd), kref_put
Origin: VMEM_IOCTL_CLOSE_DMABUF(fd)  -> unmap_attachment -> detach -> put dmabuf,
                                         debugfs del

Backstop (vmem fd close / process crash):
  vmem_import_pins_cleanup() -- sweeps all import_pins  (origin side)
  vmem_buffers_cleanup()     -- sweeps all buffer_objs  (peer side)
```

---

## Debugfs Introspection

```
/sys/kernel/debug/vmemIntel/imported   # live OPEN_DMABUF attachments (origin)
                                       # KMD imported (attached to) xe dma-buf
/sys/kernel/debug/vmemIntel/exported   # live GET_DMABUF pseudo dma-bufs (peer)
                                       # KMD exported pseudo dma-buf to daemon
```

Format — one header line per descriptor, one indented line per sg segment:

```
<pid> : fd=<fd> : size=0x<total>
       0x<phys_base_10digits> 0x<len>
       ...
```

Example:

```
# cat /sys/kernel/debug/vmemIntel/imported
4123 : fd=7 : size=0x40000000
       0x00a0000000 0x20000000
       0x00c0000000 0x20000000

# cat /sys/kernel/debug/vmemIntel/exported
4130 : fd=11 : size=0x40000000
       0x01a0000000 0x20000000
       0x01c0000000 0x20000000
```

---

## Changelog

### v4.0.0

- **Pass-through design (Method 2)**: KMD no longer performs address translation.
  `GET_DMABUF` takes pre-translated TARGET PAs from the IMEMLINK daemon (Layer A via
  Astera COSMOS SDK) and builds the MMIO pseudo dma-buf directly.
- **`vmem_ioctl_get_dmabuf_arg`**: removed `node_id` and `gpu_id` fields.
- **`vmem_pfn_entry.addr`** semantics updated: OPEN returns abs PA; GET takes TARGET PA.
- **`vmem_astera.c/.h` removed**: no in-kernel Astera lookup stub needed.
- **Debugfs segment indent**: each sg segment line is now indented 7 spaces and
  `base` is printed as a fixed-width 10-digit hex (`0x%010llx`).
- **Debugfs semantics corrected**: `imported` = origin OPEN_DMABUF records (KMD
  imported xe dma-buf); `exported` = peer GET_DMABUF records (KMD exported pseudo
  dma-buf). Call sites in `vmem_dmabuf.c` fixed accordingly.
- Version bump to 4.0.0.

### v3.1.0 — v3.1.1

- `vmem_pfn_entry.offset` renamed to `.addr`; OPEN_DMABUF returns abs PAs; UMD
  computed BAR-relative offsets (Method 1).
- `vmem_ioctl_get_dmabuf_arg` carried `node_id`/`gpu_id` for in-kernel astera_lookup.

### v3.0.0

- 4-file decomposition: `vmem_drv`, `vmem_dmabuf`, `vmem_buffer`, `vmem_debugfs`.
- All `*_IPC_HANDLE` ioctls renamed to `*_DMABUF`.
- `vmem_buffer_obj` kref lifecycle; global debugfs registry.

### v2.0.0

- PA synthesis moved from daemon into KMD (Method 1); `vmem_astera.h/c` introduced.

### v1.0.0

- BAR-relative offsets; `-ENOSPC` retry; per-fd soft-pin.
