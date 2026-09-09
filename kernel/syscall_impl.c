#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>
#include <printk.h>
#include <lib.h>
#include <panic.h>

uint64_t sys_wait()
{
        uint64_t status;
        get_arg_addr(0, &status);
        // copy_data_addr 按 uint64_t 拷贝，必须用 8 字节对齐的接收变量，
        // 不能直接对 4 字节 int 取址，否则会越界写栈上相邻内存
        uint64_t status_u = 0;
        pid_t pid = wait((int *)&status_u);
        copy_data_addr(status, &status_u);
        return pid;
}

uint64_t sys_exit()
{
        int exit_code;
        get_arg_int(0, &exit_code);
        kexit(exit_code);
        return 0; // never reaches
}

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