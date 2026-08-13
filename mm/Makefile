# mm/Makefile - 编译内存模块

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
# 根目录 Makefile
INCLUDES := -I. -I../include -I../include/mm -I../include/kernel -I../include/drivers -I../include/fs -I../include/test
CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding $(INCLUDES)  -g

SRC_C := $(wildcard *.c)
OBJS  := $(SRC_C:.c=.o)

.PHONY: all clean

all: $(OBJS)

# 模式规则：将每个 .c 编译为对应的 .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 如果你有汇编文件（如 entry.S），可以类似添加
# SRC_S := $(wildcard *.S)
# ASMOBJS := $(SRC_S:.S=.o)
# all: $(OBJS) $(ASMOBJS)

clean:
	rm -f *.o