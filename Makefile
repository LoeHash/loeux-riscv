# 根目录 Makefile顶层构建入口
CROSS_COMPILE ?= riscv64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy

ROOT := $(CURDIR)
include $(ROOT)/build/paths.mk

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
	kernel/sleeplock.o\
	kernel/proc.o\
	kernel/trap.o\
	kernel/timer.o\
	kernel/syscall_impl.o\
	kernel/syscall.o\
	kernel/syscall_file.o\
	mm/memory.o\
	mm/vm.o\
	mm/slab.o\
	mm/dma.o\
	drivers/virtio_mmio.o\
	drivers/virtio_disk.o\
	drivers/virtio_gpu.o\
	drivers/tty/uart_tty.o\
	drivers/tty.o\
	drivers/uart.o\
	fs/ext2.o\
	fs/fat32.o\
	fs/vfs.o\
	fs/char_dev.o\
	test/fdt_test.o\
	test/mem_test.o\
	test/virtio_gpu_test.o\
	test/uart_test.o\
	test/vfs_test.o\
	test/virtio_disk_test.o\
	test/vm_test.o\
	test/slab_test.o\
	utils/hashmap.o\
	font/ascii8x16.o\



TARGET := kernel.elf
TARGET_BIN := kernel.bin

# $(OBJS) 必须标记为 phony：根 make 对每个 .o 只知道 .c 显式依赖，
.PHONY: all clean boot kernel $(OBJS)

all: $(TARGET_BIN)
	$(MAKE) -C user

# 递归构建子目录
boot:
	$(MAKE) -C boot

kernel:
	$(MAKE) -C kernel


user:
	$(MAKE) -C user
	
mm:
	$(MAKE) -C mm	
drivers:
	$(MAKE) -C drivers
fs:
	$(MAKE) -C fs
asm:
	$(MAKE) -C asm	
test:
	$(MAKE) -C test

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
kernel/sleeplock.o: kernel/sleeplock.c
kernel/proc.o: kernel/proc.c
kernel/trap.o: kernel/trap.c
kernel/timer.o: kernel/timer.c
kernel/syscall_impl.o: kernel/syscall_impl.c
kernel/syscall_file.o: kernel/syscall_file.c
kernel/syscall.o: kernel/syscall.c
mm/memory.o: mm/memory.c 
mm/vm.o: mm/vm.c 
mm/slab.o: mm/slab.c 
mm/dma.o: mm/dma.c 
drivers/virtio_disk.o: drivers/virtio_disk.c 
drivers/virtio_gpu.o: drivers/virtio_gpu.c 
drivers/uart.o: drivers/uart.c 
drivers/tty/uart_tty.o: drivers/tty/uart_tty.c 
drivers/tty.o: drivers/tty.c 
drivers/virtio_mmio.o: drivers/virtio_mmio.c
fs/ext2.o: fs/ext2.c
fs/fat32.o: fs/fat32.c 
fs/vfs.o: fs/vfs.c 
fs/char_dev.o: fs/char_dev.c 
test/fdt_test.o: test/fdt_test.c 
test/mem_test.o: test/mem_test.c 
test/virtio_gpu_test.o: test/virtio_gpu_test.c 
test/uart_test.o: test/uart_test.c 
test/vfs_test.o: test/vfs_test.c 
test/virtio_disk_test.o: test/virtio_disk_test.c 
test/vm_test.o: test/vm_test.c 
test/slab_test.o: test/slab_test.c 
utils/hashmap.o: utils/hashmap.c 
font/ascii8x16.o: font/ascii8x16.c

clean:
	$(MAKE) -C boot clean
	$(MAKE) -C kernel clean 
	$(MAKE) -C drivers clean
	$(MAKE) -C fs clean
	$(MAKE) -C test clean
	$(MAKE) -C mm clean
	$(MAKE) -C user clean
	$(MAKE) -C asm clean
	rm -f $(TARGET) $(TARGET_BIN)
qemu:
	qemu-system-riscv64 \
    		-machine virt \
    		-smp 4 \
    		-m 2048M \
    		-kernel kernel.elf \
    		-device virtio-gpu-device,bus=virtio-mmio-bus.1 \
    		-display sdl \
    		-serial mon:stdio \
    		-global virtio-mmio.force-legacy=false \
    		-drive file=loeux.img,format=raw,if=none,id=loeux \
    		-device virtio-blk-device,drive=loeux,bus=virtio-mmio-bus.0


gdb:
	qemu-system-riscv64 \
    		-machine virt \
    		-smp 4 \
    		-m 2048M \
    		-kernel kernel.elf \
    		-device virtio-gpu-device,bus=virtio-mmio-bus.1 \
    		-display sdl \
    		-serial mon:stdio \
    		-global virtio-mmio.force-legacy=false \
    		-drive file=loeux.img,format=raw,if=none,id=loeux \
    		-device virtio-blk-device,drive=loeux,bus=virtio-mmio-bus.0\
		-s -S\


