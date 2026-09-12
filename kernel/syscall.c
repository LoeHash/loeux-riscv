#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>
#include <printk.h>
#include <lib.h>
#include <panic.h>

extern uint64_t sys_close();
extern uint64_t sys_open();
extern uint64_t sys_write();
extern uint64_t sys_read();
extern uint64_t sys_fork();
extern uint64_t sys_exec();
extern uint64_t sys_getpid();
extern uint64_t sys_getppid();
extern uint64_t sys_wait();
extern uint64_t sys_waitpid();
extern uint64_t sys_exit();
extern uint64_t sys_chdir();
extern uint64_t sys_mkdir();
extern uint64_t sys_fstat();
extern uint64_t sys_getdents();

static syscall_func_t syscalls[] = {
    [0] 0,                           // syscall id = 0,
    [SYSCALL_WRITE] sys_write,       // []
    [SYSCALL_FORK] sys_fork,         // []
    [SYSCALL_EXEC] sys_exec,         // []
    [SYSCALL_GETPID] sys_getpid,     // []
    [SYSCALL_GETPPID] sys_getppid,   // []
    [SYSCALL_WAIT] sys_wait,         // []
    [SYSCALL_EXIT] sys_exit,         // []
    [SYSCALL_READ] sys_read,         // []
    [SYSCALL_CHDIR] sys_chdir,       // []
    [SYSCALL_WAITPID] sys_waitpid,   // []
    [SYSCALL_MKDIR] sys_mkdir,       // []
    [SYSCALL_OPEN] sys_open,         // []
    [SYSCALL_CLOSE] sys_close,       // []
    [SYSCALL_FSTAT] sys_fstat,       // []
    [SYSCALL_GETDENTS] sys_getdents, // []
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
void do_build_user_path(char *des_path, char *u_path, char *cwd)
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