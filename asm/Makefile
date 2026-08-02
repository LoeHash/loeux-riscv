# boot/Makefile - 编译入口汇编

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding -g

.PHONY: all clean

all: kernel_trap_vec.o trampoline.o switch.o

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o