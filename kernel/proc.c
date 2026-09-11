#include <proc.h>
#include <riscv.h>
#include <stdint.h>
#include <memory.h>
#include <printk.h>
#include <panic.h>
#include <vfs.h>
#include <elf.h>
#include <spinlock.h>
#include <trap.h>
#include <lib.h>
#include <vm.h>

/*
我们现在规定，所有的用户态程序，全部在0x0 处加载运行
所以我们真正需要的就是想想kexec会做什么？
我认为kexec需要从磁盘读取文件，加载到内存，然后为进程创建页表，从0x0处开始运行
并记录进程的占用大小，此时设置进程的trapframe, epc,等必要信息
对于其他寄存器 aN ？ 我们不用管，因为我们这个进程还没运行，所以不知道寄存器
再者，即使知道寄存器，我们也不用管，因为不管寄存器的值是什么，在实际运行时都会覆盖
且，我们是刚开始运行这个进程
*/

struct task_struct tasks[NTASKS];
struct cpu cpus[NCPUS];
struct task_struct *initask = NULL;
static int init_task_startup = 0;
extern char _trampoline_jump[];
extern char _trampoline_ret[];
extern char kernel_trap_vec[];
static uint64_t pid_counter = 1;
static spinlock_t pid_lock = {0};
static uint64_t alloc_pid();
static int check_elf_header(struct elf64_ehdr *ehdr);
static void _map_user_stack(page_table pg);
static int read_phdr(int fd, uint64_t off, struct elf64_phdr *ph);
static int flags_to_pte(uint32_t p_flags);
static int load_segment(int fd, page_table pg, struct elf64_phdr *ph);
static void exit_fs(struct task_struct *ts);
static void wake_wait_parent(struct task_struct *p);

// 初始化用户第一个进程
void init_user()
{

        struct task_struct *ts;

        // alloc_task 静默获取ts锁
        // 同时不会释放
        ts = alloc_task();

        initask = ts;

        if (set_cwd(ts, "/") == -1)
        {
                panic(PANIC_ERROR, "init_tasks: set_cwd failed!\n");
        }

        ts->state = RUNNABLE;
        release(&ts->lk);
}

/// @brief 设置进程工作目录：通过 VFS 验证路径有效性
/// @param ts 目标进程
/// @param path 绝对路径（必须以 / 开头）
/// @return 0 成功, -1 路径不存在或不是目录
int set_cwd(struct task_struct *ts, const char *path)
{
        if (!path || !ts)
                return -1;

        // 通过 VFS 解析路径，获取节点
        struct vfs_node *node = vfs_lookup(path);
        if (node == NULL)
        {
                return -1; // 路径不存在
        }

        // 必须是目录
        if (!node->is_dir)
        {
                node->mount->fs_ops->fs_free_node(node->private);
                free_page(node);
                return -1;
        }

        // 释放旧的工作目录节点
        if (ts->cwd_node != NULL)
        {
                ts->cwd_node->mount->fs_ops->fs_free_node(ts->cwd_node->private);
                free_page(ts->cwd_node);
        }

        ts->cwd_node = node;
        strncpy(ts->cwd, path, 255);
        ts->cwd[255] = '\0';
        return 0;
}

/// @brief 所有fork出来的进程
///        全部会进入到此函数
void first_ret()
{
        struct task_struct *ts = get_task();

        // 来到这里，我们仍然持有
        // 此进程的锁, 此时状态必然为 running
        release(&ts->lk);

        if (!init_task_startup)
        {

                // 必须要推迟初始化
                init_task_startup = 1;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);

                // 为 init 进程建立 std fds（fd 0/1/2）。
                // 必须在 kexec 之前完成：kexec 内部会 vfs_open 目标 ELF，
                // 那次 open 会占用 ofile 的下一个空闲槽位；若先 kexec 再建 std，
                // stdin 会被 ELF 文件占用，错位。
                init_vfs_std();

                // exec
                ts->utf->a0 = kexec("/init", (char *[]){"init", NULL});
                if (ts->utf->a0 == -1)
                {
                        panic(PANIC_ERROR, "inituser: a0 is -1!\n");
                }
        }

        setup_return_trapframe(ts);
        uint64_t satp = MAKE_SATP(ts->pg);
        uint64_t trampoline_userret = TRAMPOLINE + (_trampoline_ret - _trampoline_jump);
        ((void (*)(uint64_t))trampoline_userret)(satp);
}

struct task_struct *get_task()
{
        // 修复: get_task() 可能在中断开启的上下文（如 syscall 路径）中被调用。
        // 若在 r_tp() 读取 tp 之后、访问 cpus[tp].ts 之前发生定时器中断，
        // task 可能被迁移到其他 hart，sched() 中 w_tp() 更新了 tp，
        // 导致此处用旧 tp 索引到已被 drop 的 cpus[旧hart].ts（=NULL），
        // 随后解引用引发内核态缺页。
        // 用 intr_off/intr_on 保护 tp 读取与 cpus[tp].ts 读取的原子性。
        int old = intr_get();
        intr_off();
        struct task_struct *t = get_cpu()->ts;
        if (old)
                intr_on();
        return t;
}

struct cpu *get_cpu()
{
        return &cpus[r_tp()];
}

uint64_t get_cpu_id()
{
        return r_tp();
}

/// @brief 分配任务结构体
///        此方法不会释放进程本身的锁
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

                        if ((ft->utf = (struct trapframe *)kalloc()) == NULL)
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
                        // 槽位复用：清掉上一任残留的 trap 续跑状态，
                        // 否则新任务第一次被时钟打断并 yield 后，
                        // kernel trap 路径会"恢复"上一任遗留的 sepc/sstatus
                        ft->trap_ctx_valid = 0;
                        ft->trap_sepc = 0;
                        ft->trap_sstatus = 0;
                        ft->child_exit_pending = 0;
                        ft->sleep_chan = 0;
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

        sched();

        release(&ts->lk);
}

void sched()
{
        struct task_struct *ts = get_task();
        // 是否持有当前进程的锁
        if (!is_holding(&ts->lk))
        {
                panic(PANIC_ERROR, "sched: not a owner!\n");
        }
        if (ts->state == RUNNING)
        {
                panic(PANIC_ERROR, "sched RUNNING");
        }
        if (intr_get())
                panic(PANIC_ERROR, "sched: interruptible!");

        if (get_cpu()->noff != 1)
        {
                panic(PANIC_ERROR, "sched: noff != 1");
        }
        // 执行切换
        // 把当前进程的ctx保存
        // 同时读取cpu先前的ctx
        // we got a big mistake! but fixed
        int intena = get_cpu()->intena;

        swtch(&ts->ctx, &get_cpu()->ctx);
        //
        // !!! DO NOT REMOVE !!!
        //
        // swtch 之后，当前 task 可能已经从其他 hart 迁移到了本 hart。
        // 调度器在 swtch 之前已经把 ts->utf->kernel_hartid 设置为
        // 本 hart 的 hart id，因此用它来修正 tp 寄存器。
        //
        // 否则 tp 仍然是旧 hart 的值，导致 get_cpu()/get_task() 访问
        // 错误的 per-cpu 数据，出现 cpu->ts == NULL、锁状态错乱、
        // 甚至内核态缺页等问题。
        w_tp(ts->utf->kernel_hartid);

        get_cpu()->intena = intena;
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
                found = 0;

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
                        // 首先切换状态
                        ts->state = RUNNING;
                        MEMORY_FENCE;
                        found = 1;

                        // 接下来，尽快切换
                        // 传入当前cpu上下文的存储位置
                        // 同时传入要切换的进程的ctx内核上下文
                        // 同时我们要释放锁
                        cpu->ts = ts;

                        // 当前的cpu
                        ts->utf->kernel_hartid = get_cpu_id();

                        task_restore_stvec(ts);

                        // printk("SCHED -> TASK: hart=%d pid=%d "
                        //        "task.ctx.ra=%lx task.ctx.sp=%lx "
                        //        "utf.sepc=%lx stvec=%lx sstatus=%lx\n",
                        //        get_cpu_id(),
                        //        ts->pid,
                        //        ts->ctx.ra,
                        //        ts->ctx.sp,
                        //        ts->utf->sepc,
                        //        r_stvec(),
                        //        r_sstatus());
                        swtch(&(cpu->ctx), &ts->ctx);
                        // printk("SCHED <- TASK: hart=%d pid=%d "
                        //        "state=%d ctx.ra=%lx ctx.sp=%lx "
                        //        "utf.sepc=%lx stvec=%lx sstatus=%lx\n",
                        //        get_cpu_id(),
                        //        ts->pid,
                        //        ts->state,
                        //        ts->ctx.ra,
                        //        ts->ctx.sp,
                        //        ts->utf->sepc,
                        //        r_stvec(),
                        //        r_sstatus());
                        // swtch后，说明用户程序的时间片已经
                        // 用完了，此时需要调度其他的
                        cpu->ts = 0;
                        w_stvec((uint64_t)kernel_trap_vec);
                        release(&ts->lk);
                }

                if (!found)
                {
                        // 来到这里，如果切换一圈后发现没有
                        // 进程要运行，就等一等
                        // printk("no process available! end with hart id: %d\n", get_cpu_id());
                        w_stvec((uint64_t)kernel_trap_vec);
                        intr_on();
                        asm volatile("wfi");
                        intr_off();
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

                if (kstack == NULL)
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
                ts->stvec = (uint64_t)kernel_trap_vec; // task 第一次运行时处于 kernel
                ts->trap_sepc = 0;                     // 初始没有 kernel trap
                ts->trap_sstatus = 0;                  // 初始没有 kernel trap
                ts->trap_ctx_valid = 0;                // 初始没有 kernel trap
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
        exit_fs(t);

        if (t->utf)
                kfree((void *)t->utf);
        t->utf = 0;
        if (t->pg)
                free_task_pgtable(t->pg, t->size);
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
        // TRAMPOLINE 取消映射
        // TRAPFRAME_MAPPING 取消映射
        // 但不释放对应物理页
        pg_unmap(pagetable, TRAMPOLINE, 1, 0);
        pg_unmap(pagetable, TRAPFRAME_MAPPING, 1, 0);

        // 这里就不会取消映射了
        pg_user_vmfree(pagetable, sz);
}

/// @brief 映射进程的页表, 基本映射：仅包含蹦床页和trapframe映射
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
                     (uint64_t)_trampoline_jump, PTE_V | PTE_R | PTE_X) < 0)
        {
                pg_user_vmfree(pg, 0);
                return 0;
        }
        // printk("trapframe: %0#lx\n", TRAPFRAME_MAPPING);
        if (mappages(pg, TRAPFRAME_MAPPING, PG_4K_SIZE,
                     (uint64_t)(ts->utf), PTE_R | PTE_W) < 0)
        {
                pg_unmap(pg, TRAMPOLINE, 1, 0);
                pg_user_vmfree(pg, 0);
                return 0;
        }

        return pg;
}
static void exit_fs(struct task_struct *ts)
{
        // 关闭该 task 所有打开的文件：
        // fork 时共享出去的 file->refcount 会被 file_close 的原子减抵消，
        // 最后一个引用者负责真正回收底层资源。
        for (int i = 0; i < NOFILE; i++)
        {
                struct file *f = ts->ofile[i];
                ts->ofile[i] = NULL;
                if (f)
                {
                        file_close(f);
                }
        }

        // 释放工作目录的 VFS 节点
        if (ts->cwd_node)
        {
                ts->cwd_node->mount->fs_ops->fs_free_node(ts->cwd_node->private);
                free_page(ts->cwd_node);
                ts->cwd_node = NULL;
        }
}

/// @brief 记录"有子进程退出"的 pending 标志，
///        并唤醒正睡在 wait() 里的父进程。
/// @param p 父进程
/// @note  只允许唤醒睡在 SLEEP_CHAN_CHILD 上的任务。
///        绝不能把睡在睡眠锁上的任务直接置 RUNNABLE：
///        那类任务挂在睡眠锁的等待队列上，由 release_sleep 负责
///        摘链唤醒，提前置 RUNNABLE 会让它的 sleep_node 残留在
///        队列里，之后 wakeup 摘链时会访问已脱队的节点、损坏链表。
static void wake_wait_parent(struct task_struct *p)
{
        acquire(&p->lk);
        p->child_exit_pending = 1;
        if (p->state == SLEEP && p->sleep_chan == SLEEP_CHAN_CHILD)
        {
                p->state = RUNNABLE;
        }
        release(&p->lk);
}

/// @brief 等待指定子进程退出，回收其资源
/// @param pid 要等待的子进程 pid；-1 表示等待任意子进程
/// @param status 接收子进程退出码的指针，NULL 则丢弃
/// @return 成功返回被回收的 pid；无子进程返回 -1
pid_t waitpid(pid_t pid, int *status)
{
        struct task_struct *ts = get_task();
        if (ts == NULL)
        {
                panic(PANIC_ERROR, "waitpid: ts is NULL!\n");
        }

        while (1)
        {
                struct task_struct *zombie = NULL;
                int child_count = 0;

                // 扫描所有任务，寻找当前进程的 ZOMBIE 子进程。
                // parent/state 都在 child->lk 保护下修改，必须持锁读取。
                // 注意：任一时刻最多只持有一个 child 的锁，
                // 与 kexit 的持锁顺序不构成循环等待。
                for (struct task_struct *child = tasks;
                     child < &tasks[NTASKS];
                     child++)
                {
                        acquire(&child->lk);
                        if (child->parent == ts)
                        {
                                if (pid == -1 || child->pid == pid)
                                {
                                        if (child->state == ZOMBIE)
                                        {
                                                zombie = child;
                                                break; // 持锁跳出，回收在锁内完成
                                        }
                                        child_count++;
                                }
                        }
                        release(&child->lk);
                }

                if (zombie != NULL)
                {
                        pid_t reaped = zombie->pid;
                        if (status)
                                *status = zombie->return_val;

                        // 回收 ZOMBIE 剩余资源
                        // （页表/用户内存已由 kexit 释放，这里回收 trapframe 页）
                        kfree((void *)zombie->utf);
                        zombie->utf = 0;
                        zombie->pid = 0;
                        zombie->parent = 0;
                        zombie->name[0] = 0;
                        zombie->dead = 0;

                        // trampoline是共享的, 不需要回收
                        zombie->state = INITLIZED;
                        release(&zombie->lk);
                        return reaped;
                }

                // 如果没有匹配的子进程，直接返回 -1
                if (child_count == 0)
                {
                        return -1;
                }

                // 有子进程但都没有退出：睡眠等待 kexit 唤醒。
                // pending 检查与 SLEEP 置位必须在同一临界区内完成，
                // 否则子进程恰好在这两步之间退出会造成丢失唤醒
                acquire(&ts->lk);
                if (ts->child_exit_pending)
                {
                        ts->child_exit_pending = 0;
                        release(&ts->lk);
                        continue; // 有子进程刚退出，重新扫描
                }
                ts->sleep_chan = SLEEP_CHAN_CHILD;
                ts->state = SLEEP;
                sched(); // 切出；调度器切回时已重新持有 ts->lk
                ts->sleep_chan = 0;
                release(&ts->lk);
        }
}

/// @brief 等待任意子进程退出（waitpid 的便捷包装）
pid_t wait(int *status)
{
        return waitpid(-1, status);
}

/// @brief 退出当前进程
/// @return
int kexit(int exit_code)
{
        struct task_struct *ts = get_task();
        if (ts == NULL)
        {
                panic(PANIC_ERROR, "kexit: ts is NULL!\n");
        }

        // init 进程是所有孤儿的最终收容者，绝不允许退出
        if (ts->pid == 1)
        {
                // 为了稳定性
                // 不做处理
                // panic(PANIC_ERROR, "kexit: init process can not exit!\n");
                return 0;
        }

        acquire(&ts->lk);

        // 1. 关闭所有打开的文件
        exit_fs(ts);

        // 2. 保存返回值
        ts->return_val = exit_code;

        // 3. 释放所有页表内存和物理页, 以及用户栈
        free_task_pgtable(ts->pg, ts->size);
        ts->pg = 0;
        ts->size = 0;

        // 4. 孤儿收容：所有子进程改投 init。
        //    必须先释放自身锁：否则这里"持 ts->lk 再取 child->lk"
        //    与父进程 wait() 扫描"持 child->lk 再取 ts->lk"会形成
        //    循环等待（典型场景：父子进程在不同 hart 上同时退出）。
        //    窗口期内 state 仍为 RUNNING，父进程只会把它当作
        //    存活子进程计数，不会提前回收。
        release(&ts->lk);
        uint8_t orphan_zombie = 0;
        for (struct task_struct *c = tasks; c < &tasks[NTASKS]; c++)
        {
                acquire(&c->lk);
                if (c->parent == ts)
                {
                        c->parent = initask;
                        if (c->state == ZOMBIE)
                        {
                                orphan_zombie = 1; // 有孤儿已是僵尸，通知 init 回收
                        }
                }
                release(&c->lk);
        }
        if (orphan_zombie)
        {
                wake_wait_parent(initask);
        }
        acquire(&ts->lk);

        // 5. 最后才置 ZOMBIE：
        //    父进程只能通过 acquire(ts->lk) 观察到 ZOMBIE，而本锁会
        //    一直持有到 sched() 切出、由调度器释放——这保证父进程
        //    真正拿到锁开始回收时，本任务已彻底停止运行
        //    若过早置 ZOMBIE，父进程可能在 trapframe 页仍被
        //    本任务使用时就 kfree(utf)，甚至把槽位置回 INITLIZED
        //    交给 alloc_task 复用——而本任务还在自己的内核栈上跑着。
        ts->state = ZOMBIE;
        ts->dead = 1;

        // 6. 唤醒正在 wait() 的父进程
        if (ts->parent != NULL)
        {
                wake_wait_parent(ts->parent);
        }

        // 7. 切出，永不返回
        sched();

        // 8. should never reach here
        panic(PANIC_ERROR, "kexit: sched() error! exit_code: %d!\n", exit_code);
}

/// @brief  创建一个子进程
/// @return 子进程的 pid
int kfork()
{
        pid_t new_pid, parent_pid;
        struct task_struct *new_ts, *father_ts;
        father_ts = get_task();

        if (father_ts == NULL)
        {
                panic(PANIC_ERROR, "kfork: father_ts is NULL!\n");
        }

        if ((new_ts = alloc_task()) == NULL)
        {
                return -1;
        }

        // 目前我们持有new_ts的锁
        // 1. 复制父进程页表的所有内容
        if ((vm_pagetbl_copy_asign(father_ts->pg, new_ts->pg, USER_BASE_PROG_ADDR, father_ts->size)) == -1)
        {
                // 失败路径必须释放锁，否则 free_task 之后该槽位被复用，
                // 后续 acquire 会触发 reacquire panic。
                release(&new_ts->lk);
                free_task(new_ts);
                return -1;
        }

        // 2. 映射用户栈
        if ((vm_pagetbl_copy_asign(father_ts->pg, new_ts->pg, USER_STACK_BASE, USER_STACK_SIZE)) == -1)
        {
                release(&new_ts->lk);
                free_task(new_ts);
                return -1;
        }

        // 3. 设置子进程的pid和parent
        new_ts->size = father_ts->size;
        new_ts->parent = father_ts;

        // 4. 复制name和cwd
        strcpy(new_ts->name, father_ts->name);
        if (set_cwd(new_ts, father_ts->cwd) == -1)
        {
                // cwd 设置失败（不应该发生，因为父进程的 cwd 是有效的）
                // 退回到根目录
                set_cwd(new_ts, "/");
        }

        // 5. 设置子进程的trapframe
        *(new_ts->utf) = *(father_ts->utf);
        new_ts->utf->a0 = 0;

        // 6. 复制 ofile：父子共享同一个 struct file
        //    因此每共享一次 refcount++。
        //    father 是当前运行进程，其 ofile
        //    不会被其他 hart 并发修改，
        //    但共享出的 file->refcount 必须原子自增
        //    因为后续父子任意一方 close 时会原子减
        //    避免与对方的 fork/close 竞争。
        for (int i = 0; i < NOFILE; i++)
        {
                struct file *f = father_ts->ofile[i];
                if (f)
                {
                        __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED);
                        new_ts->ofile[i] = f;
                }
                else
                {
                        new_ts->ofile[i] = NULL;
                }
        }

        new_ts->state = RUNNABLE;
        release(&new_ts->lk);

        // 注意：锁已释放，new_ts 生命周期不再受控——
        // 子进程可能已被其他 hart 调度、甚至 exit 并被回收。
        // 此处绝不能再解引用 new_ts

        return new_ts->pid;
}

int kexec(char *path, char **argv)
{
        struct elf64_ehdr ehdr;
        struct elf64_phdr phdr;

        // 获取当前task
        struct task_struct *t = get_task();

        page_table new_page = 0, old_page = t->pg;
        uint64_t user_argv_ptr[MAX_ARG_NUM];

        int fd;
        char *buf;

        fd = vfs_open(path, FS_O_READ);
        if (fd == -1)
        {
                return -1;
        }

        buf = alloc_page();
        if (buf == NULL)
        {
                // buf 本来就是 NULL，不能再 free_page（会触发 NULL 指针 panic）
                panic(PANIC_ERROR, "kexec: oom!\n");
        }

        // 先读取64字节
        if (vfs_read(fd, buf, 64) == -1)
        {
                free_page(buf);
                return -1;
        }

        int errcod;
        memcpy(&ehdr, buf, 64);
        if ((errcod = check_elf_header(&ehdr)) != 0)
        {
                panic(PANIC_ERROR, "kexec: not a efl! ");
                printk("error code: %d\n", errcod);
                return -1;
        }

        // 创建一个新的pagetable
        // 同时映射蹦床和trapframe
        new_page = create_task_pgtable(t);

        // 释放旧的pagetable
        // 旧的用户页表会被丢弃
        // 栈也会被丢弃
        // 包括蹦床页和trapframe 但只是取消映射
        // 原本的trapframe物理页并不会被free
        uint64_t old_size = t->size, new_size = 0;

        // 老的页表全部释放
        // 接下来创建新的
        // 走到这里
        // elf头部检查完毕
        // 接下来尝试加载段
        // 我们现在已经读取了前64字节
        // 下面的操作，需要计算偏移
        int seg_size_tmp;
        for (int i = 0; i < ehdr.e_phnum; i++)
        {
                read_phdr(fd, ehdr.e_phoff + i * ehdr.e_phentsize, &phdr);
                if (phdr.p_type == PT_LOAD)
                {
                        // 加载这个段
                        // 同时记录大小

                        seg_size_tmp = load_segment(fd, new_page, &phdr);
                        if (seg_size_tmp < 0)
                        {
                                free_task_pgtable(new_page, new_size);
                                return -1;
                        }
                        new_size += seg_size_tmp;
                }
        }

        // 谨记kexec的语义是将当前
        // 运行的进程全部替换
        // 前面 create_task_pgtable 已经映射好了蹦床页和trapframe
        // 接下来映射用户栈
        _map_user_stack(new_page);
        uint64_t new_sp = USER_STACK_TOP;

        // 映射参数
        int argc = 0;
        while (argv[argc])
        {
                if (argc >= MAX_ARG_NUM)
                {
                        free_task_pgtable(new_page, new_size);
                        return -1;
                }

                new_sp -= strlen(argv[argc]) + 1;
                new_sp -= new_sp % 16;

                if (new_sp < USER_STACK_BASE)
                {
                        // VERY FUCKING BAD!
                        // BUT ALMOST NEVER HAPPENS.
                        free_task_pgtable(new_page, new_size);
                        return -1;
                }

                // 将当前数据拷贝出去
                if (copyout(new_page, new_sp, argv[argc], strlen(argv[argc]) + 1) < 0)
                {
                        free_task_pgtable(new_page, new_size);
                        return -1;
                }

                user_argv_ptr[argc] = new_sp;
                argc++;
        }
        user_argv_ptr[argc] = 0;

        // 最后把整个指针数组拷贝出去
        new_sp -= (argc + 1) * sizeof(uint64_t);
        if (new_sp < USER_STACK_BASE)
        {
                free_task_pgtable(new_page, new_size);
                return -1;
        }
        if (copyout(new_page, new_sp, (char *)user_argv_ptr, (argc + 1) * sizeof(uint64_t)) < 0)
        {
                free_task_pgtable(new_page, new_size);
                return -1;
        }

        t->utf->a1 = new_sp;
        new_sp -= new_sp % 16;
        char *last, *s;
        for (last = s = path; *s; s++)
        {
                if (*s == '/')
                {
                        last = s + 1;
                }
        }
        strcpy_with_terminate(t->name, last, sizeof(t->name));

        t->pg = new_page;
        t->size = new_size;
        t->utf->sp = new_sp;
        t->utf->a0 = argc; // crt0 的 _start 直接 call main，main 从 a0 读 argc
        t->utf->sepc = ehdr.e_entry;

        free_task_pgtable(old_page, old_size);
        free_page(buf);

        return argc;
}

// 加载
static int load_segment(int fd, page_table pg, struct elf64_phdr *ph)
{
        // 无聊的检查..
        if (ph == NULL)
        {
                return -1;
        }

        char *pa;

        uint64_t start = PGROUNDDOWN(ph->p_vaddr);           // 段起始页
        uint64_t end = PGROUNDUP(ph->p_vaddr + ph->p_memsz); // 段结束页

        // BSS 边界：p_memsz > p_filesz 时，超出 filesz 的部分是 BSS（零填充、可写）。
        // 链接器可能把 .text 和 .bss 放进同一个 LOAD 段，整体标为 R+E（无 W），
        // 但 BSS 必须可写。file_end_round_down 是最后一个含文件数据页的起始，
        // 从该页起（含）需要加 PTE_W——因为该页内 file_end 之后就是 BSS。
        uint64_t file_end = ph->p_vaddr + ph->p_filesz;
        uint64_t bss_page_start = PGROUNDDOWN(file_end); // 第一个含 BSS 的页

        int seg_size = 0;
        for (uint64_t va = start; va < end; va += PG_4K_SIZE)
        {
                // 分配一个物理页
                pa = kalloc();
                if (pa == NULL)
                {
                        return -2;
                }
                memset(pa, 0, PG_4K_SIZE); // 清零

                // 计算这个页内哪些部分需要从文件读
                uint64_t page_start = MAX(va, ph->p_vaddr);
                uint64_t page_end = MIN(va + PG_4K_SIZE, file_end);

                if (page_start < page_end)
                {
                        // 这个页内有文件数据
                        uint64_t file_off = ph->p_offset + (page_start - ph->p_vaddr);
                        uint64_t len = page_end - page_start;
                        uint64_t pa_off = page_start - va;

                        vfs_seek(fd, file_off);
                        vfs_read(fd, pa + pa_off, len);
                }

                // 确定页权限：
                // - 含 BSS 的页（>= bss_page_start）必须可写
                // - 纯文件数据页用段原始权限
                int pte_flags = flags_to_pte(ph->p_flags);
                if (ph->p_memsz > ph->p_filesz && va >= bss_page_start)
                {
                        pte_flags |= PTE_W;
                }

                mappages(pg, va, PG_4K_SIZE, (uint64_t)pa, pte_flags);
                seg_size += PG_4K_SIZE;
        }
        return seg_size;
}

// 从文件偏移 off 处读取一个 Program Header
// 成功返回 0，失败返回 -1
static int read_phdr(int fd, uint64_t off, struct elf64_phdr *ph)
{
        // 跳到指定偏移
        if (vfs_seek(fd, off) < 0)
                return -1;

        // 读一个 Program Header（56 字节）
        if (vfs_read(fd, ph, sizeof(struct elf64_phdr)) != sizeof(struct elf64_phdr))
                return -1;

        return 0;
}

/// @brief 检查elf头
/// @param ehdr
/// @return
static int check_elf_header(struct elf64_ehdr *ehdr)
{
        int magic = ELF_MAGIC;
        if (memcmp(ehdr, &magic, 4) != 0)
        {
                return -1;
        }

        // 检查头大小
        if (ehdr->e_ehsize != E_EHSIZE)
        {
                return -8;
        }

        // 检查phdr条目大小
        if (ehdr->e_phentsize != E_PHENTSIZE)
        {
                return -9;
        }

        // 检查端序
        if (ehdr->e_ident[EI_DATA_OFFSET] != EI_DATA_VAL)
        {
                return -3;
        }

        // 检查class
        if (ehdr->e_ident[EI_CLASS_OFFSET] != EI_CLASS_VAL)
        {
                return -2;
        }

        // 检查版本
        if (ehdr->e_ident[EI_VERSION_OFFSET] != EI_VERSION_VAL)
        {
                return -4;
        }

        // 检查文件类型
        if (ehdr->e_type != E_TYPE)
        {
                return -5;
        }

        // 检查架构
        if (ehdr->e_machine != E_MACHINE)
        {
                return -6;
        }

        // 检查ELF版本
        if (ehdr->e_version != E_VERSION)
        {
                return -7;
        }

        // 检查段数量
        if (ehdr->e_phnum <= 0)
        {
                return -10;
        }
        return 0;
}

static int flags_to_pte(uint32_t p_flags)
{
        int perm = PTE_U; // 用户态可访问

        if (p_flags & 1) // PF_X：可执行
                perm |= PTE_X;
        if (p_flags & 2) // PF_W：可写
                perm |= PTE_W;
        if (p_flags & 4) // PF_R：可读
                perm |= PTE_R;

        return perm;
}

static void _map_user_stack(page_table pg)
{
        for (uint64_t i = USER_STACK_BASE; i < USER_STACK_TOP; i += PG_4K_SIZE)
        {
                mappages(pg, i, PG_4K_SIZE, (uint64_t)kalloc(), PTE_R | PTE_W | PTE_U);
        }
        uint64_t new_sp = USER_STACK_TOP;
}

void to_kill(struct task_struct *t)
{
        acquire(&t->lk);
        t->dead = 1;
        release(&t->lk);
}