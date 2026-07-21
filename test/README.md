# vmem driver test

Userspace smoke tests for the vmem kernel module (`/dev/vmemIntel`).

## Files

| File | Description |
|------|-------------|
| `vmem_test.c` | Ioctl smoke test — VERSION query, synthetic GET/PUT_DMABUF |
| `Makefile` | Build rules |

## Build

```bash
# Prerequisites: vmem kernel module headers in parent directory (../include/)
cd driver.gpu.vmem/test
make
```

> `vmem_test` has no Level Zero dependency. The Makefile links only against `libc`.

Output: `vmem_test`

## Running

### Prerequisites

Load the kernel module first:

```bash
cd ..
make install        # insmod vmem.ko -> /dev/vmemIntel
```

### `vmem_test` — ioctl smoke test

```bash
./vmem_test --version              # query driver version
./vmem_test --synthetic            # GET_DMABUF + PUT_DMABUF (synthetic)
./vmem_test --synthetic --node 1 --gpu 2   # specify node/gpu ids
```

**`--version`** — reads `VMEM_IOCTL_VERSION` and prints `major.minor.patch`.

**`--synthetic`** — issues `VMEM_IOCTL_GET_DMABUF` with three hard-coded
BAR-relative offset entries (0x0/4M, 0x400000/4M, 0x800000/4M) then immediately
calls `VMEM_IOCTL_PUT_DMABUF` to destroy the pseudo dma-buf.  Requires
`astera_vfe.ko` to be loaded; returns `-ENOSYS` and prints a hint if it is not.

Expected output (astera_vfe.ko loaded):

```
=== vmem driver test (v3.0) ===

[PASS] open /dev/vmemIntel (fd=3)
[PASS] VERSION: 3.1.0
[PASS] GET_DMABUF: pseudo dma-buf fd=4
[PASS] PUT_DMABUF: fd=4 destroyed
[PASS] fd=4 correctly gone

=== Test complete ===
```

Expected output (astera_vfe.ko not loaded):

```
[PASS] VERSION: 3.1.0
[FAIL] GET_DMABUF(node=0 gpu=0): Function not implemented
      Hint: load astera_vfe stub
```

## Recommended interface

For production use, prefer the `libvmem` UMD library (`umd.gpu.vmem/`) which wraps
all ioctls behind a clean C API (`vmem_open_dmabuf`, `vmem_get_dmabuf`, etc.).
`vmem_test` targets direct ioctl validation of the kernel module itself.

## Ioctl reference

See `../include/vmem_ioctl.h` for the full UAPI and `../README.md` for the
protocol description.

| Ioctl | Code | Used by this test |
|-------|------|-------------------|
| `VMEM_IOCTL_VERSION` | 0x00 | `--version` |
| `VMEM_IOCTL_GET_DMABUF` | 0x02 | `--synthetic` |
| `VMEM_IOCTL_PUT_DMABUF` | 0x03 | `--synthetic` |

## Debugfs introspection

While a test is running, the live descriptor state is visible via:

```bash
cat /sys/kernel/debug/vmemIntel/imported   # active OPEN_DMABUF pins
cat /sys/kernel/debug/vmemIntel/exported   # active GET_DMABUF pseudo dma-bufs
```

## Cleanup

```bash
make clean          # remove test binary
cd .. && make uninstall   # rmmod vmem
```
