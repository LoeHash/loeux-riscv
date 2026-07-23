# boot/Makefile - 编译入口汇编

CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
CFLAGS := -march=rv64gc -mabi=lp64 -nostdlib -ffreestanding

.PHONY: all clean

all: entry.o

entry.o: entry.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o