#include <proc.h>
#include <riscv.h>
#include <stdint.h>
#include <printk.h>
#include <panic.h>
#include <spinlock.h>
#include <trap.h>

struct task_struct tasks[NTASKS];
struct cpu cpus[NCPUS];
extern char *_trampoline_jump[];

struct cpu *get_cpu()
{
        return &cpus[r_tp()];
}

uint64_t get_cpu_id()
{
        return r_tp();
}

void init_cpu()
{
        struct cpu *now = get_cpu();
        // now->ctx = 0;
        now->ts = NULL;
        now->hart_id = r_tp();
        now->intena = 0;
        now->noff = 0;
}

void setup_trapframe()
{
}

// 初始化任务
void init_tasks()
{
        struct task_struct *ts;
        char *kstack;
        for (ts = tasks; ts < &tasks[NTASKS]; ts++)
        {

                // 重新映射一遍内核
                // 映射蹦床页
                // 映射内核栈
                // 初始化自旋锁
                // 在内核页表中映射内核栈
                kstack = alloc_page();
                if (kstack == 0)
                {
                        panic(PANIC_ERROR, "init_tasks: can not alloc page!\n");
                }
                if (kvminit(kernel_pt,
                            TASK_KERNEL_STACK(ts - tasks),
                            (phys_addr_t)kstack,
                            1,
                            PTE_V | PTE_R | PTE_W) == 0)
                {
                        panic(PANIC_ERROR, "init_tasks:kvminit() error!\n");
                }

                init_spinlock(&(ts->lk));

                ts->state = INITLIZED;
        }
}