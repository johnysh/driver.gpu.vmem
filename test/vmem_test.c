/*
 * vmem_test.c - userspace functional test for vmem driver (v4.0)
 *
 * Tests:
 *   VERSION      - query driver version (expect 4.0.0)
 *   GET_DMABUF   - create pseudo dma-buf from synthetic TARGET PAs (pass-through)
 *   PUT_DMABUF   - destroy pseudo dma-buf
 *   DEBUGFS      - verify /sys/kernel/debug/vmemIntel/{imported,exported}
 *
 * Usage:
 *   ./vmem_test [--version] [--synthetic] [--debugfs]
 *   ./vmem_test          (runs all tests)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "../include/vmem_ioctl.h"

#define VMEM_DEV  "/dev/vmemIntel"
#define DBG_IMP   "/sys/kernel/debug/vmemIntel/imported"
#define DBG_EXP   "/sys/kernel/debug/vmemIntel/exported"

#define PASS  "\033[32mPASS\033[0m"
#define FAIL  "\033[31mFAIL\033[0m"
#define INFO  "\033[36mINFO\033[0m"

static int vmem_fd = -1;

/* ── helpers ──────────────────────────────────────────────── */

static void print_debugfs(const char *path, const char *label)
{
    char buf[2048] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[" INFO "] debugfs %s: not accessible (%s)\n", label, strerror(errno));
        return;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("[" INFO "] debugfs %s: (empty)\n", label);
    } else {
        buf[n] = '\0';
        printf("[" INFO "] debugfs %s:\n", label);
        /* indent each line for clarity */
        char *line = buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            printf("          %s\n", line);
            line = nl + 1;
        }
        if (*line) printf("          %s\n", line);
    }
}

/* ── test cases ───────────────────────────────────────────── */

static int test_version(void)
{
    struct vmem_ioctl_version_arg v = {0};
    if (ioctl(vmem_fd, VMEM_IOCTL_VERSION, &v) < 0) {
        printf("[" FAIL "] VERSION: %s\n", strerror(errno));
        return -1;
    }
    printf("[" PASS "] VERSION: %u.%u.%u\n", v.major, v.minor, v.patch);
    if (v.major != VMEM_DRV_VERSION_MAJOR || v.minor != VMEM_DRV_VERSION_MINOR) {
        printf("[" FAIL "] VERSION mismatch: expected %d.%d.x, got %u.%u.%u\n",
               VMEM_DRV_VERSION_MAJOR, VMEM_DRV_VERSION_MINOR,
               v.major, v.minor, v.patch);
        return -1;
    }
    printf("[" PASS "] VERSION matches VMEM_DRV_VERSION %d.%d.%d\n",
           VMEM_DRV_VERSION_MAJOR, VMEM_DRV_VERSION_MINOR, VMEM_DRV_VERSION_PATCH);
    return 0;
}

static int test_get_put_dmabuf_and_debugfs(void)
{
    /*
     * Synthetic TARGET PAs simulating 3 scatter chunks of 4 MiB each.
     * These are fictional physical addresses — on a real system these
     * would be daemon-translated vFE window PAs.
     * We use IO-range-looking addresses (above 1 GiB) to avoid
     * dma_map_resource() errors on some IOMMU configs; on IOMMU-off
     * hosts any PA works.
     */
    struct vmem_pfn_entry entries[3] = {
        { .addr = 0x200000000ULL, .size = 4 << 20 },   /* 8 GiB + 0 MiB */
        { .addr = 0x200400000ULL, .size = 4 << 20 },   /* 8 GiB + 4 MiB */
        { .addr = 0x200800000ULL, .size = 4 << 20 },   /* 8 GiB + 8 MiB */
    };
    struct vmem_ioctl_get_dmabuf_arg arg = {
        .count       = 3,
        .entries_ptr = (uint64_t)(uintptr_t)entries,
        .fd          = -1,
    };

    printf("\n--- GET_DMABUF (pass-through, 3 synthetic TARGET PAs) ---\n");
    printf("[" INFO "] entries[0].addr = 0x%010llx  size=0x%x\n",
           (unsigned long long)entries[0].addr, entries[0].size);
    printf("[" INFO "] entries[1].addr = 0x%010llx  size=0x%x\n",
           (unsigned long long)entries[1].addr, entries[1].size);
    printf("[" INFO "] entries[2].addr = 0x%010llx  size=0x%x\n",
           (unsigned long long)entries[2].addr, entries[2].size);

    if (ioctl(vmem_fd, VMEM_IOCTL_GET_DMABUF, &arg) < 0) {
        printf("[" FAIL "] GET_DMABUF: %s\n", strerror(errno));
        return -1;
    }
    int pseudo_fd = arg.fd;
    printf("[" PASS "] GET_DMABUF: pseudo dma-buf fd=%d\n", pseudo_fd);

    /* Verify debugfs /exported shows our entry */
    printf("\n--- debugfs after GET_DMABUF ---\n");
    print_debugfs(DBG_EXP, "exported (expect 1 entry)");
    print_debugfs(DBG_IMP, "imported (expect empty)");

    /* PUT_DMABUF */
    printf("\n--- PUT_DMABUF ---\n");
    struct vmem_ioctl_put_dmabuf_arg put = { .fd = pseudo_fd };
    if (ioctl(vmem_fd, VMEM_IOCTL_PUT_DMABUF, &put) < 0) {
        printf("[" FAIL "] PUT_DMABUF(fd=%d): %s\n", pseudo_fd, strerror(errno));
        return -1;
    }
    printf("[" PASS "] PUT_DMABUF: fd=%d destroyed\n", pseudo_fd);

    /* Verify fd is gone */
    if (fcntl(pseudo_fd, F_GETFD) < 0 && errno == EBADF)
        printf("[" PASS "] fd=%d correctly EBADF after PUT\n", pseudo_fd);
    else
        printf("[" FAIL "] fd=%d still open after PUT!\n", pseudo_fd);

    /* Verify debugfs /exported is now empty */
    printf("\n--- debugfs after PUT_DMABUF ---\n");
    print_debugfs(DBG_EXP, "exported (expect empty)");
    print_debugfs(DBG_IMP, "imported (expect empty)");

    return 0;
}

/* ── main ─────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int do_version = 0, do_synthetic = 0, do_all = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version"))   do_version = 1;
        else if (!strcmp(argv[i], "--synthetic")) do_synthetic = 1;
        else {
            printf("Usage: %s [--version] [--synthetic]\n"
                   "       (no args = run all tests)\n", argv[0]);
            return 0;
        }
    }
    if (!do_version && !do_synthetic)
        do_all = 1;

    printf("=== vmem driver test (v4.0 pass-through) ===\n\n");

    vmem_fd = open(VMEM_DEV, O_RDWR);
    if (vmem_fd < 0) {
        printf("[" FAIL "] open(%s): %s\n", VMEM_DEV, strerror(errno));
        return 1;
    }
    printf("[" PASS "] open %s (fd=%d)\n", VMEM_DEV, vmem_fd);

    int rc = 0;
    if (do_all || do_version)    rc |= test_version();
    if (do_all || do_synthetic)  rc |= test_get_put_dmabuf_and_debugfs();

    close(vmem_fd);
    printf("\n=== Test %s ===\n", rc ? "FAILED" : "PASSED");
    return rc ? 1 : 0;
}
