#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>
#include <printk.h>
#include <lib.h>
#include <panic.h>
#include <memory.h>

#define MAX_PATH_LEN 128
#define MAX_ARGS 32

uint64_t sys_chdir()
{
        char path[MAX_PATH_LEN];
        uint64_t path_addr;
        get_arg_addr(0, &path_addr);
        if (copyinstr(get_task()->pg, path, path_addr, MAX_PATH_LEN) < 0)
        {
                return -1;
        }

        // 必须是绝对路径
        if (path[0] != '/')
        {
                return -1;
        }

        struct task_struct *t = get_task();
        acquire(&t->lk);
        int ret = set_cwd(t, path);
        release(&t->lk);

        return ret;
}

uint64_t sys_wait()
{
        uint64_t status;
        get_arg_addr(0, &status);
        uint64_t status_u = 0;
        pid_t pid = wait((int *)&status_u);
        copy_data_addr(status, &status_u);
        return pid;
}

uint64_t sys_waitpid()
{
        int pid;
        uint64_t status_addr;
        get_arg_int(0, &pid);
        get_arg_addr(1, &status_addr);

        uint64_t status_u = 0;
        pid_t ret = waitpid(pid, (int *)&status_u);
        if (status_addr != 0)
        {
                copy_data_addr(status_addr, &status_u);
        }
        return ret;
}

uint64_t sys_exit()
{
        int exit_code;
        get_arg_int(0, &exit_code);
        kexit(exit_code);
        return 0; // never reaches maybe.
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

/// @brief 用户态 exec 系统调用
/// @param a0 path 指针（用户空间字符串）
/// @param a1 argv 指针（用户空间 char*[]，以 NULL 结尾）
/// @return 成功不返回（用户程序被替换），失败返回 -1
/// kexec 是普通 C 函数，在内核空间正常返回。
/// 与 kexit 不同（kexit 调 sched() 切走永不返回），
/// kexec 只替换用户页表和 trapframe，内核调用链继续执行。
/// 因此 kexec 返回后可以安全地 kfree 所有内核侧分配的字符串副本。
uint64_t sys_exec()
{
        struct task_struct *ts = get_task();
        char path[MAX_PATH_LEN];
        char *argv[MAX_ARGS];
        uint64_t path_addr, argv_addr;

        get_arg_addr(0, &path_addr); // a0 = path
        get_arg_addr(1, &argv_addr); // a1 = argv[]

        // 1. 从用户空间拷贝 path 字符串
        //    copyinstr 遍历用户页表翻译地址，遇到 \0 停止
        if (copyinstr(ts->pg, path, path_addr, MAX_PATH_LEN) < 0)
        {
                return -1;
        }

        // 2. 逐个读取用户空间 argv 数组，把每个字符串拷到内核
        //    argv_addr 指向用户空间的 char*[]（8 字节指针数组）
        int argc = 0;
        int alloc_count = 0; // 已 kalloc 的数量，用于失败回滚

        while (argc < MAX_ARGS - 1)
        {
                // 从用户空间读取一个 char* 指针
                uint64_t str_addr;
                if (copyin(ts->pg, (char *)&str_addr,
                           argv_addr + argc * sizeof(uint64_t),
                           sizeof(uint64_t)) < 0)
                {
                        goto fail;
                }

                if (str_addr == 0)
                        break; // argv 结束

                // kalloc 一页内核内存存放这个字符串
                argv[argc] = kalloc();
                if (argv[argc] == NULL)
                {
                        goto fail;
                }
                alloc_count++;

                // 从用户空间拷贝字符串到内核
                if (copyinstr(ts->pg, argv[argc], str_addr, PG_4K_SIZE) < 0)
                {
                        goto fail;
                }

                argc++;
        }
        argv[argc] = NULL;

        // 3. 调用 kexec：替换用户程序
        //    kexec 内部用 copyout 把 argv 拷到新用户栈，
        //    替换页表，修改 trapframe，然后正常返回。
        int ret = kexec(path, argv);

        // 4. kexec 返回后释放所有内核侧字符串副本
        //    kexec 成功时返回 argc，失败时返回 -1。
        //    两种情况下内核副本都已无用：
        //    - 成功：字符串已被 copyout 到新用户栈
        //    - 失败：kexec 自己恢复了旧页表，直接释放
        for (int i = 0; i < alloc_count; i++)
        {
                kfree(argv[i]);
        }

        return ret;

fail:
        for (int i = 0; i < alloc_count; i++)
        {
                kfree(argv[i]);
        }
        return -1;
}