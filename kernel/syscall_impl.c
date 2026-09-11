#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>
#include <printk.h>
#include <lib.h>
#include <panic.h>
#include <memory.h>
#define BUFSZ 4096
#define MAX_PATH_LEN 128
#define MAX_ARGS 32
static void do_build_user_path(char *des_path, char *u_path, char *cwd);
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
        char u_path[MAX_PATH_LEN];
        char *argv[MAX_ARGS];
        uint64_t path_addr, argv_addr;

        get_arg_addr(0, &path_addr); // a0 = path
        get_arg_addr(1, &argv_addr); // a1 = argv[]

        // 1. 从用户空间拷贝 path 字符串
        //    copyinstr 遍历用户页表翻译地址，遇到 \0 停止
        if (copyinstr(ts->pg, u_path, path_addr, MAX_PATH_LEN) < 0)
        {
                return -1;
        }

        do_build_user_path(path, u_path, ts->cwd);

        printk("%s\n", path);

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

/**
 * do_build_user_path - 构建用户路径
 *
 * @des_path: 输出缓冲区，保存规范化后的绝对路径
 * @u_path:   用户输入路径，可为相对路径或绝对路径
 * @cwd:      当前工作目录，要求为绝对路径
 *
 * 语义：
 *   - u_path 以 '/' 开头：从根目录开始解析，忽略 cwd
 *   - 否则：从 cwd 开始解析
 *   - "."  ：当前目录，不产生变化
 *   - ".." ：返回上一级，根目录继续保持 /
 *   - 连续 '/'：视为一个 '/'
 *
 * 注意：
 *   - 不对路径进行静默截断
 *   - 如果最终路径超过 BUFSZ - 1，则返回空字符串
 *   - cwd 必须是绝对路径
 */
static void do_build_user_path(char *des_path, char *u_path, char *cwd)
{
        const char *path;
        size_t wr = 0;

        if (!des_path)
                return;

        des_path[0] = '\0';

        if (!cwd)
                return;

        /*
         * 空路径：
         * 直接返回 cwd。
         */
        if (!u_path || u_path[0] == '\0')
        {
                size_t cwd_len = strlen(cwd);

                if (cwd_len >= BUFSZ)
                        return;

                memcpy(des_path, cwd, cwd_len);
                des_path[cwd_len] = '\0';
                return;
        }

        /*
         * 确定解析起点。
         *
         * 绝对路径：
         *     /a/b
         *       ^
         *
         * 相对路径：
         *     cwd/a/b
         */
        if (u_path[0] == '/')
        {
                /*
                 * 从根开始。
                 */
                des_path[wr++] = '/';
                path = u_path;

                /*
                 * 跳过开头所有 '/'。
                 */
                while (*path == '/')
                        path++;
        }
        else
        {
                size_t cwd_len = strlen(cwd);

                /*
                 * cwd 必须是绝对路径。
                 */
                if (cwd_len == 0 || cwd[0] != '/')
                        return;

                /*
                 * 先把 cwd 放入结果。
                 *
                 * 这里不直接 memcpy 全部 cwd，
                 * 而是按照 segment 写入，保证 cwd 自己的
                 * 尾 '/' 不会导致重复 '/'。
                 */
                if (cwd_len >= BUFSZ)
                        return;

                memcpy(des_path, cwd, cwd_len);
                wr = cwd_len;

                /*
                 * 去掉 cwd 尾部多余 '/'。
                 *
                 * "/" 是特殊情况，不能变成空字符串。
                 */
                while (wr > 1 && des_path[wr - 1] == '/')
                        wr--;

                des_path[wr] = '\0';

                path = u_path;
        }

        while (*path)
        {
                const char *seg;
                size_t seg_len;

                /*
                 * 跳过连续 '/'。
                 */
                while (*path == '/')
                        path++;

                if (*path == '\0')
                        break;

                /*
                 * 当前 segment。
                 */
                seg = path;

                while (*path && *path != '/')
                        path++;

                seg_len = (size_t)(path - seg);

                /*
                 * "."：
                 * 什么都不做。
                 */
                if (seg_len == 1 && seg[0] == '.')
                {
                        continue;
                }

                /*
                 * ".."：
                 * 返回上一级。
                 */
                if (seg_len == 2 && seg[0] == '.' && seg[1] == '.')
                {
                        /*
                         * 根目录不能继续往上。
                         */
                        if (wr > 1)
                        {
                                /*
                                 * 先去掉当前最后一个 segment。
                                 *
                                 * 例如：
                                 *
                                 * /home/user
                                 *          ^
                                 *
                                 * 变成：
                                 *
                                 * /home
                                 */
                                while (wr > 1 && des_path[wr - 1] != '/')
                                        wr--;

                                /*
                                 * 如果最后剩下的是 '/'，保留它。
                                 */
                                if (wr > 1 && des_path[wr - 1] == '/')
                                        wr--;

                                des_path[wr] = '\0';
                        }

                        continue;
                }

                /*
                 * 普通 segment。
                 *
                 * 如果当前不是根目录，需要先补 '/'。
                 */
                if (wr > 1)
                {
                        if (wr + 1 >= BUFSZ)
                        {
                                des_path[0] = '\0';
                                return;
                        }

                        des_path[wr++] = '/';
                }

                /*
                 * 检查 segment 是否放得下。
                 */
                if (wr + seg_len >= BUFSZ)
                {
                        des_path[0] = '\0';
                        return;
                }

                memcpy(des_path + wr, seg, seg_len);
                wr += seg_len;

                des_path[wr] = '\0';
        }

        /*
         * 最终保证：
         *
         *   "/"     -> "/"
         *   "/a"    -> "/a"
         *   "/a/b"  -> "/a/b"
         */
        des_path[wr] = '\0';
}