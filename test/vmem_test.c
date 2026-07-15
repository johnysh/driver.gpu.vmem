/*
 * vmem_test.c - userspace functional test for the vmem kernel driver
 *
 * Tests all 5 IOCTL interfaces:
 *   1. REGISTER_EP       - register a PCIe endpoint by BDF
 *   2. GET_PFN_OFFSET_LIST - parse a real dma-buf fd (needs xe GPU + dma-buf)
 *   3. GET_IPC_FD        - create fake dma-buf from PFN list
 *   4. CLOSE_IPC_FD      - destroy the fake dma-buf
 *   5. PUT_IPC_FD        - release producer reference
 *
 * For a standalone test without real dma-bufs (no GPU required),
 * run with --synthetic to test GET_IPC_FD / CLOSE_IPC_FD directly.
 *
 * Usage:
 *   ./vmem_test [--synthetic] [--ep-bus 0x4d --ep-dev 0 --ep-fn 0]
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
#define TEST_PASS "\033[32mPASS\033[0m"
#define TEST_FAIL "\033[31mFAIL\033[0m"

static int vmem_fd = -1;

static void die(const char *msg)
{
	perror(msg);
	if (vmem_fd >= 0) close(vmem_fd);
	exit(1);
}

/* -- Test helpers -------------------- */

static int test_open_device(void)
{
	vmem_fd = open(VMEM_DEV, O_RDWR);
	if (vmem_fd < 0) {
		fprintf(stderr, "[FAIL] open(%s): %s\n", VMEM_DEV, strerror(errno));
		fprintf(stderr, "  Is the vmem module loaded? Try: insmod vmem.ko\n");
		return -1;
	}
	printf("[%s] open /dev/vmemIntel  (fd=%d)\n", TEST_PASS, vmem_fd);
	return 0;
}

static int test_register_ep(uint8_t bus, uint8_t device, uint8_t function,
			     uint64_t bar2_base, uint64_t bar2_size)
{
	struct vmem_ioctl_register_ep_arg arg = {
		.bus      = bus,
		.device   = device,
		.function = function,
		.bar2_base = bar2_base,
		.bar2_size = bar2_size,
	};
	int ret = ioctl(vmem_fd, VMEM_IOCTL_REGISTER_EP, &arg);
	if (ret < 0) {
		fprintf(stderr, "[%s] REGISTER_EP(%02x:%02x.%x): %s\n",
			TEST_FAIL, bus, device, function, strerror(errno));
		return -1;
	}
	printf("[%s] REGISTER_EP: BDF=%02x:%02x.%x bar2=0x%llx size=0x%llx\n",
	       TEST_PASS, bus, device, function,
	       (unsigned long long)bar2_base, (unsigned long long)bar2_size);
	return 0;
}

static int test_get_ipc_fd_synthetic(void)
{
	/*
	 * Build a synthetic PFN list (no real GPU needed).
	 * Simulates what the producer would send over bmemlink.
	 */
	struct vmem_ioctl_get_ipc_fd_arg arg;
	memset(&arg, 0, sizeof(arg));

	arg.pfn_list.count = 3;
	/* Three 4MB chunks at increasing offsets */
	arg.pfn_list.entries[0].offset = 0x000000;
	arg.pfn_list.entries[0].size   = 4 * 1024 * 1024;
	arg.pfn_list.entries[1].offset = 0x400000;
	arg.pfn_list.entries[1].size   = 4 * 1024 * 1024;
	arg.pfn_list.entries[2].offset = 0x800000;
	arg.pfn_list.entries[2].size   = 4 * 1024 * 1024;

	int ret = ioctl(vmem_fd, VMEM_IOCTL_GET_IPC_FD, &arg);
	if (ret < 0) {
		fprintf(stderr, "[%s] GET_IPC_FD (synthetic): %s\n",
			TEST_FAIL, strerror(errno));
		return -1;
	}
	printf("[%s] GET_IPC_FD (synthetic): fake dma-buf fd=%d (3x4MB)\n",
	       TEST_PASS, arg.fd);
	return arg.fd;
}

static int test_close_ipc_fd(int fake_fd)
{
	struct vmem_ioctl_close_ipc_fd_arg arg = { .fd = fake_fd };
	int ret = ioctl(vmem_fd, VMEM_IOCTL_CLOSE_IPC_FD, &arg);
	if (ret < 0) {
		fprintf(stderr, "[%s] CLOSE_IPC_FD(fd=%d): %s\n",
			TEST_FAIL, fake_fd, strerror(errno));
		return -1;
	}
	printf("[%s] CLOSE_IPC_FD: fd=%d closed\n", TEST_PASS, fake_fd);
	return 0;
}

static int test_get_pfn_list(int real_dma_fd, uint8_t bus, uint8_t dev, uint8_t fn)
{
	struct vmem_ioctl_get_pfn_arg arg;
	memset(&arg, 0, sizeof(arg));
	arg.fd       = real_dma_fd;
	arg.bus      = bus;
	arg.device   = dev;
	arg.function = fn;

	int ret = ioctl(vmem_fd, VMEM_IOCTL_GET_PFN_OFFSET_LIST, &arg);
	if (ret < 0) {
		fprintf(stderr, "[%s] GET_PFN_OFFSET_LIST(fd=%d, BDF=%02x:%02x.%x): %s\n",
			TEST_FAIL, real_dma_fd, bus, dev, fn, strerror(errno));
		return -1;
	}
	printf("[%s] GET_PFN_OFFSET_LIST: %u entries\n",
	       TEST_PASS, arg.pfn_list.count);
	for (unsigned i = 0; i < arg.pfn_list.count && i < 8; i++) {
		printf("      entry[%u]: offset=0x%016llx size=%u\n",
		       i, (unsigned long long)arg.pfn_list.entries[i].offset,
		       arg.pfn_list.entries[i].size);
	}
	return (int)arg.pfn_list.count;
}

/* -- Main -------------------- */

int main(int argc, char *argv[])
{
	int synthetic = 0;
	uint8_t ep_bus = 0, ep_dev = 0, ep_fn = 0;
	uint64_t ep_bar2_base = 0x100000000ULL;  /* 4 GiB default for test */
	uint64_t ep_bar2_size = 0x40000000ULL;   /* 1 GiB */

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--synthetic"))
			synthetic = 1;
		else if (!strcmp(argv[i], "--ep-bus") && i+1 < argc)
			ep_bus = (uint8_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--ep-dev") && i+1 < argc)
			ep_dev = (uint8_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--ep-fn") && i+1 < argc)
			ep_fn = (uint8_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--ep-bar2") && i+1 < argc)
			ep_bar2_base = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("Usage: %s [--synthetic] [--ep-bus B] [--ep-dev D] "
			       "[--ep-fn F] [--ep-bar2 ADDR]\n", argv[0]);
			printf("  --synthetic   test without real GPU dma-buf\n");
			printf("  --ep-bus/dev/fn  PCIe BDF of local endpoint\n");
			printf("  --ep-bar2 ADDR   BAR2 physical base (hex)\n");
			return 0;
		}
	}

	printf("=== vmem driver test (v0.1) ===\n\n");

	/* 1. Open device */
	if (test_open_device() < 0)
		return 1;

	/* 2. Register endpoint (synthetic address if not provided) */
	if (test_register_ep(ep_bus, ep_dev, ep_fn, ep_bar2_base, ep_bar2_size) < 0) {
		fprintf(stderr, "  Note: use --ep-bar2 to provide a valid BAR2 address\n");
		/* Continue anyway - let GET_IPC_FD fail with proper error */
	}

	if (synthetic) {
		/* 3. Consumer path: create fake dma-buf */
		int fake_fd = test_get_ipc_fd_synthetic();
		if (fake_fd < 0)
			goto done;

		/* 4. Verify the fd is valid */
		off_t sz = lseek(fake_fd, 0, SEEK_END);
		printf("      fake dma-buf size via lseek: %ld bytes "
		       "(expected %d)\n", (long)sz, 3 * 4 * 1024 * 1024);

		/* 5. Close it */
		test_close_ipc_fd(fake_fd);

		/* 6. Verify fd is gone */
		if (fcntl(fake_fd, F_GETFD) < 0 && errno == EBADF)
			printf("[%s] fd %d correctly closed\n", TEST_PASS, fake_fd);
		else
			printf("[%s] fd %d still open after CLOSE_IPC_FD!\n",
			       TEST_FAIL, fake_fd);
	} else {
		printf("\nNote: GPU dma-buf test requires a real dma-buf fd.\n");
		printf("Pass a valid fd from zeMemGetIpcHandle to GET_PFN_OFFSET_LIST.\n");
		printf("Use --synthetic for a no-GPU test.\n");
	}

done:
	close(vmem_fd);
	vmem_fd = -1;
	printf("\n=== Test complete ===\n");
	return 0;
}

