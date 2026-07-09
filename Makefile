# Makefile for vmem kernel module
#
# Usage:
#   make              - build the kernel module
#   make clean        - remove build artifacts
#   make install      - insmod the module
#   make uninstall    - rmmod the module
#   make info         - show module info

obj-m += vmem.o
vmem-objs := vmem_drv.o vmem_dmabuf.o

# Extra cflags: include the local include/ directory for vmem_ioctl.h
ccflags-y := -I$(src) -DDEBUG

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	@echo "Loading vmem kernel module..."
	@if lsmod | grep -q "^vmem "; then \
		echo "vmem already loaded, reloading..."; \
		sudo rmmod vmem; \
	fi
	sudo insmod vmem.ko
	@echo "vmem loaded. Device: $$(ls -la /dev/vmem)"

uninstall:
	@if lsmod | grep -q "^vmem "; then \
		sudo rmmod vmem; \
		echo "vmem unloaded"; \
	else \
		echo "vmem not loaded"; \
	fi

info: vmem.ko
	modinfo vmem.ko

.PHONY: all clean install uninstall info
