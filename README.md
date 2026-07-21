# vMem KMD — PCIe Cross-Node GPU dma-buf Translator

**Version:** 3.1.0 | **License:** GPL-2.0 | **Device:** `/dev/vmemIntel`

## Overview

vMem is a Linux kernel module that enables cross-node GPU memory sharing over a PCIe Gen5
switch (Astera PEX890xx). It exposes a `/dev/vmemIntel` misc device and implements both
sides of the IPC buffer exchange — no modifications to the Intel xe GPU driver required.

**Method 1 (this build): address translation in KMD via Astera vFE kernel API.**

- **Origin node**: attach GPU dma-buf (xe-exported), extract BAR2-relative scatter
  offsets, keep soft-pin to prevent TTM eviction.
- **Peer node**: call `astera_lookup(node_id, gpu_id)` in-kernel to obtain the vFE
  endpoint, compute absolute PAs, export MMIO-backed pseudo dma-buf for P2P DMA.

---

## Architecture

```
Origin Node                                    Peer Node
─────────────────────────────────────────      ──────────────────────────────────────
GPU App                                        GPU App
  │ allocate VRAM                                │ import pseudo dma-buf → P2P R/W
  │ zeMemGetIpcHandle() → dma-buf fd             ↑
  ↓                                              │ zeMemOpenIpcHandle(pseudo_fd)
libvmem (UMD)                                  libvmem (UMD)
  │ VMEM_IOCTL_OPEN_DMABUF(dmabuf_fd, bdf)       │ VMEM_IOCTL_GET_DMABUF
  ↓                                              │   input: {node_id, gpu_id, offsets}
vMem KMD (/dev/vmemIntel)                      vMem KMD (/dev/vmemIntel)
  │ dma_buf_get() → dma_buf_dynamic_attach()     │ astera_lookup(node_id, gpu_id)
  │ dma_buf_map_attachment() → sg_table          │   -> (dbdf, bar, index)
  │ returns abs PA per chunk                     │ vfe_base = BAR_start + index × 32 GB
  │   (sg_dma_address, IOMMU-off = PA)           │ PA'[i] = vfe_base + offset[i]
  │ soft-pin (keep attach+sgt alive)             │ dma_buf_export() → pseudo_fd
  │ debugfs: imported                            │ vmem_buffer_obj tracked (kref)
  ↓                                              │ debugfs: exported
libvmem (UMD) receives {abs PA[], count}        ↓
  │ bar2_base = sysfs_read_bar2(bdf)           xe GPU driver
  │ offset[i] = abs_pa[i] - bar2_base
  │
  ·─── control channel: {node_id, gpu_id, offset list} ──→ libvmem (UMD) Peer
                                                                    │ (calls GET_DMABUF above)
                                               Astera PCIe Switch (PEX890xx)
                                               routes P2P DMA to origin VRAM
```

### Module Decomposition

| File | Responsibility |
|------|----------------|
| `vmem_drv.c` | Module init/exit, misc device registration, `open`/`release`, ioctl dispatch. Sets 64-bit DMA mask. Calls `vmem_debugfs_init/exit`. |
| `vmem_dmabuf.c` | **Export path**: P2P attach → map → absolute PA extraction (sg_dma_address) → soft-pin; UMD computes BAR-relative offsets. **Import path**: astera_lookup → PA synthesis → MMIO dma-buf → `vmem_buffer_obj` tracking. Debugfs hooks on each open/get and close/put. |
| `vmem_buffer.c` | `vmem_buffer_obj` lifecycle: `kref` init/get/put → release frees segs + obj. Per-fd `buffers` list for backstop cleanup. |
| `vmem_debugfs.c` | Global `imported`/`exported` registry (two lists + one mutex). Read-only seq_file debugfs files under `/sys/kernel/debug/vmemIntel/`. |
| `vmem_astera.c` | Spinlock-protected function-pointer registration stub. Exports `vmem_register/unregister_astera_lookup` so `astera_vfe.ko` can plug in at runtime. Returns `-ENOSYS` until registered. |

---

## Repository Layout

```
driver.gpu.vmem/
├── include/
│   └── vmem_ioctl.h        UAPI — include in UMD (libvmem)
├── vmem_astera.h/c         Astera vFE registration stub
├── vmem_buffer.h/c         vmem_buffer_obj kref lifecycle
├── vmem_debugfs.h/c        Global registry + debugfs
├── vmem_dmabuf.h/c         Export + import dma-buf operations
├── vmem_drv.c              Device registration, ioctl dispatch
├── Makefile
├── README.md
└── test/
    ├── README.md           Test directory documentation
    └── vmem_test.c         Ioctl smoke test (VERSION, GET/PUT_DMABUF)
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

> **IOMMU:** Set `intel_iommu=pt` (passthrough) so DMA addresses equal physical addresses.

---

## IOCTL API

Include `include/vmem_ioctl.h`. Magic: `'V'` (0x56). Version: **3.1.0**.

### Command Table

| Command | Code | Direction | Description |
|---------|------|-----------|-------------|
| `VMEM_IOCTL_VERSION` | 0x00 | `_IOR` | Query driver version |
| `VMEM_IOCTL_OPEN_DMABUF` | 0x01 | `_IOWR` | Origin: P2P attach, walk sg_table, return ABSOLUTE PAs in entries[].addr. UMD computes offset[i]=addr[i]-bar2_base |
| `VMEM_IOCTL_GET_DMABUF` | 0x02 | `_IOWR` | Peer: astera_lookup → PA synthesis → pseudo dma-buf fd |
| `VMEM_IOCTL_PUT_DMABUF` | 0x03 | `_IOW` | Peer: destroy pseudo dma-buf fd |
| `VMEM_IOCTL_CLOSE_DMABUF` | 0x04 | `_IOW` | Origin: release soft-pin (by opened fd) |

> Every object is identified by a **dma-buf fd** — no opaque handles.

### Two-step PFN Retrieval (`OPEN_DMABUF`)

```
1st call: count = 0
  -> KMD returns -ENOSPC, count = required N
Allocate: entries = malloc(N * sizeof(vmem_pfn_entry))
2nd call: count = N, entries_ptr = entries
  -> KMD fills entries[i].{addr, size}
```

### Key Structures

```c
/* One scatter chunk */
struct vmem_pfn_entry {
    __u64 addr;  /* abs PA (OPEN) or BAR-relative offset (GET) */
    __u32 size;    /* page-aligned byte length */
    __u32 pad;
};

/* OPEN_DMABUF */
struct vmem_ioctl_open_dmabuf_arg {
    __s32  fd;           /* in:  GPU dma-buf fd */
    __u8   bus, device, function, pad2;  /* in: GPU BDF */
    __u32  count;        /* in/out: capacity / actual count (-ENOSPC retry) */
    __u32  page_size;    /* out: PAGE_SIZE */
    __u64  total_size;   /* out: total bytes across all chunks */
    __u64  entries_ptr;  /* in: userspace ptr -> vmem_pfn_entry[] output */
};

/* GET_DMABUF (Method 1) */
struct vmem_ioctl_get_dmabuf_arg {
    __u32  node_id;      /* in:  remote node id (from origin control channel) */
    __u32  gpu_id;       /* in:  remote GPU id */
    __u32  count;        /* in:  number of BAR-relative entries */
    __u32  pad;
    __u64  entries_ptr;  /* in:  vmem_pfn_entry[] (BAR-relative offsets from origin) */
    __s32  fd;           /* out: pseudo dma-buf fd */
    __u32  pad2;
};
```

---

## Usage Example (Peer Side)

```c
int vmem_fd = open("/dev/vmemIntel", O_RDWR);

/* Receive from origin over control channel */
struct vmem_pfn_entry *entries;
uint32_t count, node_id, gpu_id;
recv_from_daemon(&entries, &count, &node_id, &gpu_id);

/* GET_DMABUF: KMD does astera_lookup + PA synthesis internally */
struct vmem_ioctl_get_dmabuf_arg arg = {
    .node_id     = node_id,
    .gpu_id      = gpu_id,
    .count       = count,
    .entries_ptr = (uint64_t)(uintptr_t)entries,
};
if (ioctl(vmem_fd, VMEM_IOCTL_GET_DMABUF, &arg) < 0) {
    if (errno == ENOSYS) /* astera_vfe.ko not loaded */
    perror("GET_DMABUF"); goto out;
}

/* Import pseudo dma-buf into local GPU */
ze_ipc_mem_handle_t ipc = {};
memcpy(ipc.data, &arg.fd, sizeof(int));
zeMemOpenIpcHandle(ctx, peer_gpu, ipc, 0, &remote_ptr);

/* ... P2P DMA (Astera switch routes to origin VRAM) ... */

/* Teardown */
zeMemCloseIpcHandle(ctx, remote_ptr);
struct vmem_ioctl_put_dmabuf_arg put = { .fd = arg.fd };
ioctl(vmem_fd, VMEM_IOCTL_PUT_DMABUF, &put);
out:
close(vmem_fd);
```

---

## Lifetime & Cleanup

```
Peer:   VMEM_IOCTL_PUT_DMABUF(fd)   -> remove vmem_buffer_obj from list,
                                        debugfs del, close_fd(fd), kref_put
Origin: VMEM_IOCTL_CLOSE_DMABUF(fd) -> unmap -> unpin -> detach -> put,
                                        debugfs del

Backstop (vmem fd close / process crash):
  vmem_import_pins_cleanup() -- sweeps all import_pins
  vmem_buffers_cleanup()     -- sweeps all buffer_objs (close_fd + kref_put each)
```

---

## Debugfs Introspection

```
/sys/kernel/debug/vmemIntel/imported   # live OPEN_DMABUF attachments (origin)
/sys/kernel/debug/vmemIntel/exported   # live GET_DMABUF pseudo dma-bufs (peer)
```

Format (one block per descriptor):
```
<pid> : fd=<fd> : size=0x<total>
0x<phys_base> 0x<len>
...
```

Example:
```
# cat /sys/kernel/debug/vmemIntel/exported
4130 : fd=11 : size=0x40000000
0x01a0000000 0x20000000
0x01c0000000 0x20000000
```

---

## Astera vFE Driver Integration

### Module load order

```
vmem.ko          loads first  -- exports vmem_register_astera_lookup
astera_vfe.ko    loads second -- calls vmem_register_astera_lookup(my_fn)
astera_vfe.ko    unloads      -- calls vmem_unregister_astera_lookup(my_fn)
```

`GET_DMABUF` returns `-ENOSYS` until `astera_vfe.ko` registers.

### `astera_vfe.ko` implementation template

```c
#include <vmem_astera.h>

static int my_lookup(u32 node_id, u32 gpu_id, struct astera_vfe_ep *ep)
{
    struct my_vfe_record *r = vfe_table_lookup(node_id, gpu_id);
    if (!r) return -ENODEV;
    ep->domain = r->pci_domain;
    ep->bus    = r->pci_bus;
    ep->devfn  = PCI_DEVFN(r->slot, r->fn);
    ep->bar    = 2;               /* BAR2 = LMEM window on PEX890xx */
    ep->index  = r->window_idx;  /* 32 GiB slot within BAR2 */
    return 0;
}

static int __init astera_vfe_init(void)
{ vmem_register_astera_lookup(my_lookup); return 0; }
static void __exit astera_vfe_exit(void)
{ vmem_unregister_astera_lookup(my_lookup); }
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
| `bar` | BAR index (2 = LMEM window on PEX890xx) |
| `index` | Window slot; `base = pci_resource_start(dbdf, bar) + index × 32 GiB` |

---

## Changelog

### v3.1.1

- **`vmem.h` C++ linkage fix**: `extern C {` corrected to `extern "C" {`; C++ consumers now compile correctly
- **GET_DMABUF fd cleanup**: `copy_to_user` failure in `vmem_ioctl_get_dmabuf` now calls `vmem_put_dmabuf()` to release the installed fd instead of leaking it until the vmem fd is closed
- **`vmem_gpu_test.c` removed**: stale v3.0 integration test (used obsolete ioctls and `.offset` field); replaced by `umd.gpu.vmem/test/vmem_p2p_test.c` which uses the libvmem API

### v3.1.0

- **mem_pfn_entry.offset renamed to .addr**: OPEN_DMABUF now returns absolute physical addresses; UMD computes BAR-relative offsets (offset = abs_pa - gpu_bar2_base)
- **KMD no longer reads GPU BAR2 base** (removed from kernel; UMD reads sysfs)
- **OPEN_DMABUF arg gains page_size and total_size output fields**
- Version bump to 3.1.0

### v3.0.0

- **4-file module decomposition** per HLD: `vmem_drv`, `vmem_dmabuf`, `vmem_buffer`, `vmem_debugfs`
- **API renamed**: all `*_IPC_HANDLE` → `*_DMABUF` / `*_DMABUF_FD`; objects keyed by dma-buf fd (no opaque handles)
- **`vmem_buffer.c`**: new — `vmem_buffer_obj` lifecycle with `kref`; tracks every `GET_DMABUF` result
- **`vmem_debugfs.c`**: new — global imported/exported registry; seq_file over `/sys/kernel/debug/vmemIntel/`
- **`vmem_file_priv`**: gains `buffers` list + `buf_lock` for peer-side pseudo dma-bufs
- **`vmem_buffers_cleanup()`**: new backstop sweeps exported bufs on fd close/crash
- **`IDENTIFY_DMABUF` ioctl removed** per HLD
- **`vmem_get_dmabuf_fd()`**: returns `int fd` (not `dma_buf *`); adds priv arg for tracking

### v2.2.0

- Restored full origin side: `OPEN_IPC_HANDLE` / `CLOSE_IPC_HANDLE`, soft-pin machinery
- per-fd pin tracking via `vmem_file_priv`; fd-close auto-sweeps remaining pins

### v2.0.0

- PA synthesis moved from daemon into KMD (Method 1)
- New `vmem_astera.h/c` registration stub

### v1.0.0

- BAR-relative offsets; `-ENOSPC` retry; per-fd soft-pin
