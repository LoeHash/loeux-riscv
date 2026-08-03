#include <proc.h>
#include <riscv.h>
#include <stdint.h>
#include <printk.h>
#include <panic.h>
#include <spinlock.h>
#include <trap.h>
#include <vm.h>

struct task_struct tasks[NTASKS];
struct cpu cpus[NCPUS];
extern char *_trampoline_jump[];

struct task_struct *get_task()
{
        return get_cpu()->ts;
}

struct cpu *get_cpu()
{
        return &cpus[r_tp()];
}

uint64_t get_cpu_id()
{
        return r_tp();
}
/// yield 是 进程表明自己可以切换 切换到cpu调度器主循环
/// yield 加的锁和 调度器加的锁不构成冲突
/// 这里的锁 是在sched间传递的
void yield()
{
        struct task_struct *ts = get_task();

        // 需要加锁
        acquire(&ts->lk);
        if (ts->state != RUNNING)
        {
                release(&ts->lk);
                return;
        }

        ts->state = RUNNABLE;

        // 切换
        sched();
        release(&ts->lk);
}

void sched()
{
        // 是否持有当前进程的锁
        if (!is_holding(&get_task()->lk))
        {
                panic(PANIC_ERROR, "sched: not a owner!\n");
        }

        // 执行切换
        // 把当前进程的ctx保存
        // 同时读取cpu先前的ctx
        swtch(&get_cpu()->ctx, &get_task()->ctx);
}

/// 而对于cpu来说，cpu的内核态上下文实际上就是调度器的代码
/// 而进程的内核上下文，可能是除了调度器之外的任意代码的位置
/// 对于这个，整个的cpu执行流程是很复杂的
/// 我们永远无法得知进入scheduler之前cpu在执行什么
/// 但永远谨记 cpu->ctx 是cpu的当前上下文
/// task_struct->ctx 是进程的在内核态里的上下文
void scheduler()
{
        struct task_struct *ts;
        struct cpu *cpu = &cpus[r_tp()];
        uint8_t found = 0;
        // printk("enter the scheduler with hart id: %d\n", get_cpu_id());
        printk("sstatus = %#lx\n", r_sstatus());
        printk("sie     = %#lx\n", r_sie());
        while (1)
        {
                // 调度
                printk("finding the runnable task... hart id: %d\n", get_cpu_id());
                for (ts = tasks; ts < &tasks[NTASKS]; ts++)
                {
                        if (ts->state != RUNNABLE)
                        {
                                continue;
                        }

                        // 多个核心，存在竞争条件
                        // 进程必须在退出内核态前
                        // 释放掉自身的锁
                        acquire(&ts->lk);

                        if (ts->state != RUNNABLE)
                        {
                                release(&ts->lk);
                                continue;
                        }
                        found = 1;
                        // 首先切换状态
                        ts->state = RUNNING;

                        // 接下来，尽快切换
                        // 传入当前cpu上下文的存储位置
                        // 同时传入要切换的进程的ctx内核上下文
                        // 同时我们要释放锁
                        cpu->ts = ts;
                        // 当前的cpu
                        ts->utf->kernel_hartid = get_cpu_id();
                        swtch(&(cpu->ctx), &ts->ctx);

                        // swtch后，说明用户程序的时间片已经
                        // 用完了，此时需要调度其他的
                        cpu->ts = 0;

                        release(&ts->lk);
                }

                if (!found)
                {
                        // 来到这里，如果切换一圈后发现没有
                        // 进程要运行，就等一等
                        // printk("no process available! end with hart id: %d\n", get_cpu_id());
                        asm volatile("wfi");
                }
        }
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

// 初始化任务
void init_tasks()
{
        struct task_struct *ts;
        char *kstack;

        for (ts = tasks; ts < &tasks[NTASKS]; ts++)
        {

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