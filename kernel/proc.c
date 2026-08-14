#include <proc.h>
#include <riscv.h>
#include <stdint.h>
#include <memory.h>
#include <printk.h>
#include <panic.h>
#include <spinlock.h>
#include <trap.h>
#include <lib.h>
#include <vm.h>

struct task_struct tasks[NTASKS];
struct cpu cpus[NCPUS];
extern char *_trampoline_jump[];
extern char *_trampoline_ret[];
static uint64_t pid_counter = 1;
static spinlock_t pid_lock = {0};
static uint64_t alloc_pid();

/// @brief 所有fork出来的进程
///        全部会进入到此函数
void first_ret()
{
        struct task_struct *ts = get_task();

        // 来到这里，我们仍然持有
        // 此进程的锁, 此时状态必然为 running
        release(&ts->lk);

        // 若是第一个进程

        setup_return_trapframe(ts);
        uint64_t satp = MAKE_SATP(ts->pg);
        uint64_t trampoline_userret = TRAMPOLINE + (_trampoline_ret - _trampoline_jump);
        ((void (*)(uint64_t))trampoline_userret)(satp);
}

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

// 分配任务结构体
struct task_struct *alloc_task()
{
        struct task_struct *ft;

        for (ft = tasks; ft < &tasks[NTASKS]; ft++)
        {
                acquire(&ft->lk);
                if (ft->state == INITLIZED)
                {
                        ft->pid = alloc_pid();
                        ft->state = USED;

                        if ((ft->utf = (struct trapframe *)kalloc()) == 0)
                        {
                                free_task(ft);
                                release(&ft->lk);
                                return 0;
                        }

                        // An empty user page table.
                        ft->pg = create_task_pgtable(ft);
                        if (ft->pg == 0)
                        {
                                free_task(ft);
                                release(&ft->lk);
                                return 0;
                        }

                        memset(&ft->ctx, 0, sizeof(ft->ctx));
                        ft->ctx.ra = (uint64_t)first_ret;
                        // 进程内核栈，而不是cpu调度器栈
                        ft->ctx.sp = ft->kstack;
                        return ft;
                }
                else
                {
                        release(&ft->lk);
                }
        }

        return 0;
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
        while (1)
        {
                // 调度
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
                            PTE_V | PTE_R | PTE_W, 0) == 0)
                {
                        panic(PANIC_ERROR, "init_tasks:kvminit() error!\n");
                }

                init_spinlock(&(ts->lk));

                ts->state = INITLIZED;
                ts->kstack = TASK_KERNEL_STACK(ts - tasks);
        }
}

static uint64_t alloc_pid()
{
        uint64_t tmp;
        acquire(&pid_lock);
        tmp = pid_counter;
        pid_counter++;
        release(&pid_lock);
        return tmp;
}

void free_task(struct task_struct *t)
{
        if (t->utf)
                kfree((void *)t->utf);
        t->utf = 0;
        if (t->pg)
                task_freepagetable(t->pg, t->size);
        t->pg = 0;
        t->size = 0;
        t->pid = 0;
        t->parent = 0;
        t->name[0] = 0;
        t->dead = 0;
        // t->xstate = 0;
        t->state = INITLIZED;
}

void free_task_pgtable(page_table pagetable, uint64_t sz)
{
        // 如果这里选择释放物理页
        // 则触发 refree panic
        pg_unmap(pagetable, TRAMPOLINE, 1, 0);
        pg_unmap(pagetable, TRAPFRAME_MAPPING, 1, 0);
        pg_user_vmfree(pagetable, sz);
}

/// @brief 映射进程的页表, 基本映射
/// @param ts
/// @return
page_table create_task_pgtable(struct task_struct *ts)
{
        page_table pg;
        pg = pg_create();
        if (pg == 0)
        {
                return 0;
        }

        if (mappages(pg, TRAMPOLINE, PG_4K_SIZE,
                     (uint64_t)_trampoline_jump, PTE_R | PTE_X) < 0)
        {
                pg_user_vmfree(pg, 0);
                return 0;
        }

        if (mappages(pg, TRAPFRAME_MAPPING, PG_4K_SIZE,
                     (uint64_t)(ts->utf), PTE_R | PTE_W) < 0)
        {
                pg_unmap(pg, TRAMPOLINE, 1, 0);
                pg_user_vmfree(pg, 0);
                return 0;
        }

        return pg;
}
