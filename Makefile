# 根目录 Makefile顶层构建入口
CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy

INCLUDES := -I. \
            -I./include \
            -I./include/kernel \
            -I./include/mm \
            -I./include/drivers \
            -I./include/fs \
            -I./kernel \
            -I./mm \
            -I./boot \
            -I./asm \
	    -I./fs \
	    -I./drivers\
ARCH := rv64gc
ABI := lp64
CFLAGS := -g -march=$(ARCH) -mabi=$(ABI) -nostdlib -ffreestanding $(INCLUDES) -Wall -Werro
LDFLAGS := -T kernel/kernel.lds -nostdlib

	
OBJS := asm/kernel_trap_vec.o\
	asm/trampoline.o\
	asm/switch.o\
	boot/entry.o\
	boot/sec_entry.o\
	kernel/start.o\
	kernel/printk.o\
	kernel/fdt.o\
	kernel/panic.o\
	kernel/spinlock.o\
	kernel/proc.o\
	kernel/trap.o\
	kernel/timer.o\
	mm/memory.o\
	mm/vm.o\
	drivers/virtio_disk.o\
	drivers/uart.o\
	fs/fat12.o\
	fs/vfs.o\
	fs/char_dev.o\


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
drivers:
	$(MAKE) -C drivers
fs:
	$(MAKE) -C fs
asm:
	$(MAKE) -C asm	


$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^


$(TARGET_BIN): $(TARGET)
	$(OBJCOPY) -O binary $< $@


$(OBJS):
	$(MAKE) -C $(dir $@)


asm/kernel_trap_vec.o: asm/kernel_trap_vec.S
asm/trampoline.o: asm/trampoline.S
asm/switch.o: asm/switch.S
boot/entry.o: boot/entry.S
boot/sec_entry.o: boot/sec_entry.S

kernel/start.o: kernel/start.c 
kernel/printk.o: kernel/printk.c
kernel/fdt.o: kernel/fdt.c
kernel/panic.o: kernel/panic.c
kernel/spinlock.o: kernel/spinlock.c
kernel/proc.o: kernel/proc.c
kernel/trap.o: kernel/trap.c
kernel/trap.o: kernel/timer.c
mm/memory.o: mm/memory.c 
mm/vm.o: mm/vm.c 
drivers/virtio_disk.o: drivers/virtio_disk.c 
drivers/uart.o: drivers/uart.c 
fs/fat12.o: fs/fat12.c 
fs/vfs.o: fs/vfs.c 
fs/char_dev.o: fs/char_dev.c 


clean:
	$(MAKE) -C boot clean
	$(MAKE) -C kernel clean
	$(MAKE) -C drivers clean
	$(MAKE) -C fs clean
	$(MAKE) -C mm clean
	$(MAKE) -C asm clean
	rm -f $(TARGET) $(TARGET_BIN)
qemu:
	make -j16 && qemu-system-riscv64 \
			-machine virt \
			-smp 4 \
			-m 2048M \
			-kernel ./kernel.elf \
			-nographic \
			-global virtio-mmio.force-legacy=false\
			-drive file=./loeux.img,format=raw,if=none,id=loeux \
			-device virtio-blk-device,drive=loeux,bus=virtio-mmio-bus.0\
			-serial mon:stdio\

gdb:
	qemu-system-riscv64 -machine virt -smp 4 -m 2048M -nographic -kernel ./kernel.elf -s -S


