#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>
#include <printk.h>
#include <lib.h>
#include <panic.h>

extern uint64_t sys_write();
extern uint64_t sys_fork();
extern uint64_t sys_exec();
extern uint64_t sys_getpid();
extern uint64_t sys_getppid();
extern uint64_t sys_wait();
extern uint64_t sys_exit();
extern uint64_t sys_read();

static syscall_func_t syscalls[] = {
    [0] 0,                         // syscall id = 0,
    [SYSCALL_WRITE] sys_write,     // []
    [SYSCALL_FORK] sys_fork,       // []
    [SYSCALL_EXEC] sys_exec,       // []
    [SYSCALL_GETPID] sys_getpid,   // []
    [SYSCALL_GETPPID] sys_getppid, // []
    [SYSCALL_WAIT] sys_wait,       // []
    [SYSCALL_EXIT] sys_exit,       // []
    [SYSCALL_READ] sys_read};

static uint64_t get_arg_reg(int n)
{
        struct task_struct *t = get_task();
        switch (n)
        {
        case 0:
                return t->utf->a0;
        case 1:
                return t->utf->a1;
        case 2:
                return t->utf->a2;
        case 3:
                return t->utf->a3;
        case 4:
                return t->utf->a4;
        case 5:
                return t->utf->a5;
        }
        return -1;
}

int copy_data_addr(uint64_t addr, uint64_t *ip)
{
        struct task_struct *t = get_task();
        // 将内核数据 *ip 写入用户虚拟地址 addr（copyout：内核 → 用户）。
        // 之前误用 copyin（用户 → 内核），方向相反：
        // 会把用户栈上的旧值覆盖进内核变量，wait 的 status 永远传不出去。
        // 映射合法性由 copyout 遍历页表验证，未映射地址自然失败。
        return copyout(t->pg, addr, (char *)ip, sizeof(*ip));
}

/// @brief 从内核缓冲区 buf 复制 max 字节到用户虚拟地址 addr 中
/// @param addr 用户虚拟地址
/// @param buf 内核缓冲区
/// @param max 最大复制字节数
/// @return 实际复制字节数
int copy_data_str_out(uint64_t addr, char *buf, int max)
{
        struct task_struct *t = get_task();
        if (copyout(t->pg, addr, buf, max) < 0)
        {
                return -1;
        }
        return max;
}

/// @brief 从用户虚拟地址 addr 复制 max 字节到 buf 中
/// @param addr 用户虚拟地址
/// @param buf 内核缓冲区
/// @param max 最大复制字节数
/// @return 实际复制字节数
int copy_data_str(uint64_t addr, char *buf, int max)
{
        struct task_struct *t = get_task();
        if (copyin(t->pg, buf, addr, max) < 0)
                return -1;
        return strlen(buf);
}

void get_arg_addr(int n, uint64_t *buf)
{
        *buf = get_arg_reg(n);
}

void get_arg_int(int n, int *buf)
{
        *buf = (int)get_arg_reg(n);
}

void syscall()
{
        struct task_struct *ts = get_task();

        if (ts == 0)
        {
                panic(PANIC_ERROR, "syscall: ts == NULL!\n");
        }

        int sys_id = ts->utf->a7;

        if (sys_id > 0 && syscalls[sys_id] && sys_id < ARR_LEN(syscalls))
        {
                ts->utf->a0 = syscalls[sys_id]();
        }
        else
        {
                ts->utf->a0 = -1;
        }
}