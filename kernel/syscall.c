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

static syscall_func_t syscalls[] = {
    [0] 0,                     // syscall id = 0,
    [SYSCALL_WRITE] sys_write, // []
    [SYSCALL_FORK] sys_fork,   // []
};

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
        if (addr >= t->size || addr + sizeof(uint64_t) > t->size)
        {
                return -1;
        }

        if (copyin(t->pg, (char *)ip, addr, sizeof(*ip)) != 0)
        {
                return -1;
        }

        return 0;
}

int copy_data_str(uint64_t addr, char *buf, int max)
{
        struct task_struct *t = get_task();
        if (copyinstr(t->pg, buf, addr, max) < 0)
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