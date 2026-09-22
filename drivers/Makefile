# drivers/Makefile

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
ROOT := $(abspath $(CURDIR)/..)
include $(ROOT)/build/paths.mk

CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding $(INCLUDES) -g -O2
CFLAGS += -MMD -MP

# 子目录列表
SUBDIRS := $(wildcard */)

# 当前目录的源文件
SRC_C := $(wildcard *.c)
OBJS  := $(SRC_C:.c=.o)
SRC_S := $(wildcard *.S)
ASMOBJS := $(SRC_S:.S=.o)

.PHONY: all clean $(SUBDIRS)

all: $(OBJS) $(ASMOBJS) $(SUBDIRS)


$(SUBDIRS):
	$(MAKE) -C $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.d
	for d in $(SUBDIRS); do $(MAKE) -C $$d clean; done

-include $(OBJS:.o=.d)