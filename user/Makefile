# user/Makefile - 用户程序编译模块
CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld

INCLUDES := -I. -I../include -I../include/mm -I../include/kernel -I../include/drivers -I../include/fs -I../include/test

CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding $(INCLUDES) -g
ASFLAGS := -march=rv64gc -mabi=lp64 $(INCLUDES) -g

CRT0 := crt0.o

IMAGE := ../loeux.img
MOUNT_POINT := /media/loe

LIB_SRCS ?= ulib.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
USER_LIBS ?= $(LIB_OBJS)

PROG_SRCS := $(filter-out $(LIB_SRCS), $(wildcard *.c))
PROGS := $(addprefix _, $(PROG_SRCS:.c=))

.PHONY: all clean

all: $(PROGS)
	@if mount | grep -q "$(MOUNT_POINT)"; then sudo umount $(MOUNT_POINT) 2>/dev/null || true; fi
	sudo mount $(IMAGE) $(MOUNT_POINT)
# 	sudo mkdir -p $(MOUNT_POINT)/bin
	sudo cp $(PROGS) $(MOUNT_POINT)/
	@sync
	@cd / && sudo umount $(MOUNT_POINT) 2>/dev/null || true

_%: %.o $(CRT0) $(USER_LIBS) user.ld
	$(LD) -T user.ld -o $@ $< $(CRT0) $(USER_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

clean:
	rm -f *.o $(PROGS)