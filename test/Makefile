CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2 -I.. -I/usr/include
LDFLAGS = -lze_loader -ldl

TARGETS = vmem_test
SRCS    = vmem_test.c

all: $(TARGETS)

vmem_test: vmem_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
