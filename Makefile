# 根目录 Makefile顶层构建入口
CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
# 根目录 Makefile
INCLUDES := -I. \
            -I./include \
            -I./include/kernel \
            -I./include/mm \
            -I./kernel \
            -I./mm \
            -I./boot \
            -I./asm
ARCH := rv64gc
ABI := lp64
CFLAGS := -g -march=$(ARCH) -mabi=$(ABI) -nostdlib -ffreestanding $(INCLUDES) -Wall -Werro
LDFLAGS := -T kernel/kernel.lds -nostdlib

	
OBJS := asm/kernel_trap_vec.o\
	asm/trampoline.o\
	boot/entry.o\
	boot/sec_entry.o\
	kernel/start.o\
	kernel/printk.o\
	kernel/fdt.o\
	kernel/panic.o\
	kernel/spinlock.o\
	kernel/proc.o\
	kernel/trap.o\
	mm/memory.o\
	mm/vm.o\
	


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
asm:
	$(MAKE) -C asm	

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
asm/kernel_trap_vec.o: asm/kernel_trap_vec.S
asm/trampoline.o: asm/trampoline.S
boot/entry.o: boot/entry.S
boot/sec_entry.o: boot/sec_entry.S

kernel/start.o: kernel/start.c 
kernel/printk.o: kernel/printk.c
kernel/fdt.o: kernel/fdt.c
kernel/panic.o: kernel/panic.c
kernel/spinlock.o: kernel/spinlock.c
kernel/proc.o: kernel/proc.c
kernel/trap.o: kernel/trap.c
mm/memory.o: mm/memory.c 
mm/vm.o: mm/vm.c 

clean:
	$(MAKE) -C boot clean
	$(MAKE) -C kernel clean
	$(MAKE) -C mm clean
	rm -f $(TARGET) $(TARGET_BIN)
qemu:
	make -j16 && qemu-system-riscv64 -machine virt -smp 4 -m 2048M -nographic -kernel /work/os/loeux-riscv/kernel.elf

gdb:
	qemu-system-riscv64 -machine virt -smp 4 -m 2048M -nographic -kernel /work/os/loeux-riscv/kernel.elf -s -S
