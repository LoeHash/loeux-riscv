# user/Makefile - 用户程序编译模块
CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld

INCLUDES := -I. -I../include -I../include/mm -I../include/kernel -I../include/drivers -I../include/fs -I../include/test

CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding $(INCLUDES) -g
ASFLAGS := -march=rv64gc -mabi=lp64 $(INCLUDES) -g

CRT0 := crt0.o

# ==================== 用户库配置 ====================
# 库源文件（这些文件不会被当作独立用户程序，而是编译后链接到所有用户程序）
# 示例：如果你有一个 ulib.c，取消下面一行的注释即可
# LIB_SRCS := ulib.c

LIB_SRCS ?=
LIB_OBJS := $(LIB_SRCS:.c=.o)

USER_LIBS ?= $(LIB_OBJS)

# ==================== 用户程序 ====================
PROG_SRCS := $(filter-out $(LIB_SRCS), $(wildcard *.c))

PROGS := $(addprefix _, $(PROG_SRCS:.c=))

.PHONY: all clean

all: $(PROGS)

# 链接规则：每个用户程序单独链接
_%: %.o $(CRT0) $(USER_LIBS) user.ld
	$(LD) -T user.ld -o $@ $< $(CRT0) $(USER_LIBS)

# 编译规则
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

clean:
	rm -f *.o $(PROGS)