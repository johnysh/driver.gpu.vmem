# Makefile for vmem kernel module (v3.0)
# Four source files + astera stub
obj-m += vmem.o
vmem-objs := vmem_drv.o vmem_dmabuf.o vmem_buffer.o vmem_debugfs.o

ccflags-y := -I$(src) -DDEBUG

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	@if lsmod | grep -q "^vmem "; then rmmod vmem; fi
	insmod vmem.ko

uninstall:
	@if lsmod | grep -q "^vmem "; then rmmod vmem && echo "vmem unloaded"; fi

info: vmem.ko
	modinfo vmem.ko

.PHONY: all clean install uninstall info
