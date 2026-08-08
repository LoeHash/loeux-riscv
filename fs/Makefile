# kernel/Makefile - 编译所有内核C文件

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
INCLUDES := -I. -I../include -I../include/kernel -I../include/mm -I../mm -I../include/drivers -I../include/fs
CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding $(INCLUDES) -g

SRC_C := $(wildcard *.c)
OBJS  := $(SRC_C:.c=.o)
SRC_S := $(wildcard *.S)
ASMOBJS := $(SRC_S:.S=.o)

.PHONY: all clean


all: $(OBJS) $(ASMOBJS)

# 模式规则：将每个 .c 编译为对应的 .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o