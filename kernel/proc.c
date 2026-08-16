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
extern char *_trampoline_jump[];
extern char *_trampoline_ret[];
static uint64_t pid_counter = 1;
static spinlock_t pid_lock = {0};
static uint64_t alloc_pid();
static int check_elf_header(struct elf64_ehdr *ehdr);
static int read_phdr(int fd, uint64_t off, struct elf64_phdr *ph);
static int flags_to_pte(uint32_t p_flags);
static int load_segment(int fd, page_table pg, struct elf64_phdr *ph);

// 初始化用户第一个进程
void init_user()
{

        struct task_struct *ts;

        // alloc_task 静默获取ts锁
        // 同时不会释放
        ts = alloc_task();

        initask = ts;

        set_cwd(ts, "/");

        ts->state = RUNNABLE;
        release(&ts->lk);
}

/// @brief 设置进程的工作目录, path必须以\0结尾
/// @param ts
/// @param path
void set_cwd(struct task_struct *ts, char *path)
{
        // 设置工作目录
        memcpy(ts->cwd, path, strlen(path) + 1);
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

                // exec
                ts->utf->a0 = kexec("/init", (char *[]){"hello!", 0});
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

        fd = vfs_open(path, FS_MODE_READ);
        if (fd == -1)
        {
                return -1;
        }

        buf = alloc_page();
        if (buf == NULL)
        {
                free_page(buf);
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
                panic(PANIC_WRONG, "kexec: not a efl! error code: %d\n", errcod);
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
        for (uint64_t i = USER_STACK_BASE; i < USER_STACK_TOP; i += PG_4K_SIZE)
        {
                mappages(new_page, i, PG_4K_SIZE, (uint64_t)kalloc(), PTE_R | PTE_W | PTE_U);
        }
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
                uint64_t page_start = MAX(va, ph->p_vaddr);         // 本页内段的起始
                uint64_t file_end = ph->p_vaddr + ph->p_filesz;     // 文件数据结束
                uint64_t page_end = MIN(va + PG_4K_SIZE, file_end); // 本页内文件数据结束

                if (page_start < page_end)
                {
                        // 这个页内有文件数据
                        uint64_t file_off = ph->p_offset + (page_start - ph->p_vaddr); // 对应文件偏移
                        uint64_t len = page_end - page_start;                          // 拷贝长度
                        uint64_t pa_off = page_start - va;                             // 物理页内偏移

                        // 从文件读数据到物理页的正确位置
                        vfs_seek(fd, file_off);
                        vfs_read(fd, pa + pa_off, len);
                }

                // 映射物理页到虚拟地址
                mappages(pg, va, PG_4K_SIZE, (uint64_t)pa, flags_to_pte(ph->p_flags));
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

        // 检查class
        if (ehdr->e_ident[EI_CLASS_OFFSET] != EI_CLASS_VAL)
        {
                return -2;
        }

        // 检查端序
        if (ehdr->e_ident[EI_DATA_OFFSET] != EI_DATA_VAL)
        {
                return -3;
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
        int perm = PTE_U; // 用户态可访问（用户程序必须设置）

        if (p_flags & 1) // PF_X：可执行
                perm |= PTE_X;
        if (p_flags & 2) // PF_W：可写
                perm |= PTE_W;
        if (p_flags & 4) // PF_R：可读
                perm |= PTE_R;

        return perm;
}