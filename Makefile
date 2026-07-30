# 根目录 Makefile顶层构建入口
CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy

ARCH := rv64gc
ABI := lp64
CFLAGS := -g -march=$(ARCH) -mabi=$(ABI) -nostdlib -ffreestanding -Iinclude
LDFLAGS := -T kernel/kernel.lds -nostdlib

OBJS := boot/entry.o\
	kernel/start.o\
	kernel/printk.o\
	kernel/fdt.o\
	kernel/panic.o\
	mm/memory.o\
	mm/vm.o


TARGET := kernel.elf
TARGET_BIN := kernel.bin

.PHONY: all clean boot kernel

all: $(TARGET_BIN)

# 递归构建子目录
boot:
	$(MAKE) -C boot

kernel:
	$(MAKE) -C kernel
	
mm:
	$(MAKE) -C mm	

# 链接
$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# 生成二进制
$(TARGET_BIN): $(TARGET)
	$(OBJCOPY) -O binary $< $@

# 规则：进入子目录编译目标文件
$(OBJS):
	$(MAKE) -C $(dir $@)

# 手工保持依赖关系（暂时不自动生成 .d 文件）
boot/entry.o: boot/entry.S
kernel/start.o: kernel/start.c kernel/sbi.h kernel/printk.h 
kernel/printk.o: kernel/printk.c kernel/printk.h
kernel/fdt.o: kernel/fdt.c kernel/fdt.h
kernel/panic.o: kernel/panic.c kernel/panic.h
mm/memory.o: mm/memory.c mm/memory.h
mm/vm.o: mm/vm.c mm/vm.h

clean:
	$(MAKE) -C boot clean
	$(MAKE) -C kernel clean
	$(MAKE) -C mm clean
	rm -f $(TARGET) $(TARGET_BIN)
qemu:
	make -j16 && qemu-system-riscv64 -machine virt -smp 1 -m 2048M -nographic -kernel /work/os/loeux-riscv/kernel.elf

gdb:
	qemu-system-riscv64 -machine virt -smp 1 -m 2048M -nographic -kernel /work/os/loeux-riscv/kernel.elf -s -S
