#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>
#include <printk.h>

static syscall_func_t syscalls[] = {
    [0] 0 // syscall id = 0
          // []
};

void syscall()
{
        printk("退出了!\n");
        while (1)
                ;
}