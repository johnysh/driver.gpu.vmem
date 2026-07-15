/*
 * vmem_test.c - userspace functional test for vmem driver (v3.0)
 *
 * VERSION      - query driver version
 * GET_DMABUF   - create pseudo dma-buf (synthetic, needs astera_vfe stub)
 * PUT_DMABUF   - destroy pseudo dma-buf
 *
 * Usage:
 *   ./vmem_test --version
 *   ./vmem_test --synthetic [--node N --gpu G]
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

#define VMEM_DEV "/dev/vmemIntel"
#define PASS "\033[32mPASS\033[0m"
#define FAIL "\033[31mFAIL\033[0m"

static int vmem_fd = -1;

static int test_version(void)
{
    struct vmem_ioctl_version_arg v = {0};
    if (ioctl(vmem_fd, VMEM_IOCTL_VERSION, &v) < 0) {
        fprintf(stderr, "[" FAIL "] VERSION: %s\n", strerror(errno));
        return -1;
    }
    printf("[" PASS "] VERSION: %u.%u.%u\n", v.major, v.minor, v.patch);
    return 0;
}

static int test_get_dmabuf(uint32_t node_id, uint32_t gpu_id)
{
    struct vmem_pfn_entry entries[3] = {
        { .offset = 0x000000ULL, .size = 4 << 20 },
        { .offset = 0x400000ULL, .size = 4 << 20 },
        { .offset = 0x800000ULL, .size = 4 << 20 },
    };
    struct vmem_ioctl_get_dmabuf_arg arg = {
        .node_id     = node_id,
        .gpu_id      = gpu_id,
        .count       = 3,
        .entries_ptr = (uint64_t)(uintptr_t)entries,
        .fd          = -1,
    };
    if (ioctl(vmem_fd, VMEM_IOCTL_GET_DMABUF, &arg) < 0) {
        fprintf(stderr, "[" FAIL "] GET_DMABUF(node=%u gpu=%u): %s\n",
                node_id, gpu_id, strerror(errno));
        if (errno == ENOSYS)
            fprintf(stderr, "      Hint: load astera_vfe stub\n");
        return -1;
    }
    printf("[" PASS "] GET_DMABUF: pseudo dma-buf fd=%d\n", arg.fd);
    return arg.fd;
}

static int test_put_dmabuf(int pseudo_fd)
{
    struct vmem_ioctl_put_dmabuf_arg arg = { .fd = pseudo_fd };
    if (ioctl(vmem_fd, VMEM_IOCTL_PUT_DMABUF, &arg) < 0) {
        fprintf(stderr, "[" FAIL "] PUT_DMABUF(fd=%d): %s\n",
                pseudo_fd, strerror(errno));
        return -1;
    }
    printf("[" PASS "] PUT_DMABUF: fd=%d destroyed\n", pseudo_fd);
    if (fcntl(pseudo_fd, F_GETFD) < 0 && errno == EBADF)
        printf("[" PASS "] fd=%d correctly gone\n", pseudo_fd);
    else
        printf("[" FAIL "] fd=%d still open!\n", pseudo_fd);
    return 0;
}

int main(int argc, char *argv[])
{
    int do_version = 0, do_synthetic = 0;
    uint32_t node_id = 0, gpu_id = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version"))        do_version = 1;
        else if (!strcmp(argv[i], "--synthetic")) do_synthetic = 1;
        else if (!strcmp(argv[i], "--node") && i+1 < argc) node_id = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--gpu")  && i+1 < argc) gpu_id  = strtoul(argv[++i], NULL, 0);
        else { printf("Usage: %s [--version] [--synthetic [--node N --gpu G]]\n", argv[0]); return 0; }
    }
    if (!do_version && !do_synthetic) do_version = 1;

    printf("=== vmem driver test (v3.0) ===\n\n");
    vmem_fd = open(VMEM_DEV, O_RDWR);
    if (vmem_fd < 0) {
        fprintf(stderr, "[" FAIL "] open(%s): %s\n", VMEM_DEV, strerror(errno));
        return 1;
    }
    printf("[" PASS "] open %s (fd=%d)\n", VMEM_DEV, vmem_fd);

    if (do_version)   test_version();
    if (do_synthetic) {
        int fd = test_get_dmabuf(node_id, gpu_id);
        if (fd >= 0) test_put_dmabuf(fd);
    }

    close(vmem_fd);
    printf("\n=== Test complete ===\n");
    return 0;
}
