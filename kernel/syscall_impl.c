#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>
#include <printk.h>
#include <lib.h>
#include <panic.h>

uint64_t sys_getppid()
{
        if (get_task()->parent == NULL && get_task()->pid == 1)
        {
                return 0;
        }
        return get_task()->parent->pid;
}

uint64_t sys_getpid()
{
        return get_task()->pid;
}

uint64_t sys_fork()
{
        return kfork();
}