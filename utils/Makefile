# utils/Makefile - 编译所有工具库C文件

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
ROOT := $(abspath $(CURDIR)/..)
include $(ROOT)/build/paths.mk
CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding $(INCLUDES) -g -O2
# -MMD -MP: 生成 .d 头文件依赖，头文件改动时能自动重编对应的 .o，
# 避免 struct 布局变更后新旧 .o 混编
CFLAGS += -MMD -MP

SRC_C := $(wildcard *.c)
OBJS  := $(SRC_C:.c=.o)
SRC_S := $(wildcard *.S)
ASMOBJS := $(SRC_S:.S=.o)

.PHONY: all clean


all: $(OBJS) $(ASMOBJS)

# 模式规则：将每个 .c 编译为对应的 .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -f *.o *.d