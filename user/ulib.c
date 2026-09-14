#include <ulib.h>

// STDIO //////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////

int fstat(int fd, struct stat *buf)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "li a7, %3\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(fd), "r"(buf), "i"(SYSCALL_FSTAT)
            : "a0", "a1", "a7", "memory");
        return ret;
}

// getdents：从目录 fd 读取若干定长 struct dirent。
// 返回填入 buf 的字节数；目录读完返回 0；出错返回 -1。
int getdents(int fd, struct dirent *buf, uint32_t count)
{
        int ret;
        // 显式 mv 到 a0/a1/a2：不能依赖 "r" 约束自动分配
        // （read/write/exec 都踩过参数落到 a3/a4/a5 的坑）。
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "mv a2, %3\n"
            "li a7, %4\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(fd), "r"(buf), "r"(count), "i"(SYSCALL_GETDENTS)
            : "a0", "a1", "a2", "a7", "memory");
        return ret;
}

int close(int fd)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "li a7, %2\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(fd), "i"(SYSCALL_CLOSE)
            : "a0", "a7", "memory");
        return ret;
}

int open(const char *path, int flags)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "li a7, %3\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(path), "r"(flags), "i"(SYSCALL_OPEN)
            : "a0", "a1", "a7", "memory");
        return ret;
}

int mkdir(const char *path)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "li a7, %2\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(path), "i"(SYSCALL_MKDIR)
            : "a0", "a7", "memory");
        return ret;
}

int chdir(const char *path)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "li a7, %2\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(path), "i"(SYSCALL_CHDIR)
            : "a0", "a7", "memory");
        return ret;
}

int read(int fd, void *buf, uint64_t count)
{
        int ret;
        // 必须显式把参数放进 a0/a1/a2：
        // 用 "r" 约束编译器会自由分配寄存器（之前发现 fd/buf/count
        // 被放进 a3/a4/a5，ecall 时内核从 a0/a1/a2 取到的是垃圾值）。
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "mv a2, %3\n"
            "li a7, %4\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(fd), "r"(buf), "r"(count), "i"(SYSCALL_READ)
            : "a0", "a1", "a2", "a7", "memory");
        return ret;
}

int get_pid()
{
        int pid;

        __asm__ volatile(
            "li a7, %1\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(pid)
            : "i"(SYSCALL_GETPID)
            : "a0", "a7", "memory");

        return pid;
}

int get_ppid()
{
        int ppid;

        __asm__ volatile(
            "li a7, %1\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ppid)
            : "i"(SYSCALL_GETPPID)
            : "a0", "a7", "memory");

        return ppid;
}

int fork()
{
        int ret;

        __asm__ volatile(
            "li a7, %1\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "i"(SYSCALL_FORK)
            : "a0", "a7", "memory");

        return ret;
}

int exec(const char *path, char **argv)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "li a7, %3\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(path), "r"(argv), "i"(SYSCALL_EXEC)
            : "a0", "a1", "a7", "memory");
        return ret;
}

int wait(int *status)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "li a7, %2\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(status), "i"(SYSCALL_WAIT)
            : "a0", "a7", "memory");
        return ret;
}

int waitpid(int pid, int *status)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "li a7, %3\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(pid), "r"(status), "i"(SYSCALL_WAITPID)
            : "a0", "a1", "a7", "memory");
        return ret;
}

int write(int fd, void *buf, uint64_t count)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "mv a2, %3\n"
            "li a7, %4\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(fd), "r"(buf), "r"(count), "i"(SYSCALL_WRITE)
            : "a0", "a1", "a2", "a7", "memory");
        return ret;
}

int exit(int exit_code)
{
        __asm__ volatile(
            "li a7, %1\n"
            "ecall\n"
            :
            : "r"(exit_code), "i"(SYSCALL_EXIT)
            : "a0", "a7", "memory");
        return 0;
}

int pwd(char *buf, int max)
{
        int ret;
        __asm__ volatile(
            "mv a0, %1\n"
            "mv a1, %2\n"
            "li a7, %3\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(buf), "r"(max), "i"(SYSCALL_PWD)
            : "a0", "a1", "a7", "memory");
        return ret;
}

// ============================ PATH 搜索 ============================
// 分层说明：内核的 exec() 只负责“给定一个路径 → 加载”，相对路径按 cwd 解析；
// “去哪些目录找命令”属于策略，放用户态（类 POSIX：execve 不搜 PATH，
// execvp 才搜）。这样内核 syscall 接口保持最小、稳定，PATH 也能按进程定制。

#define EXEC_PATH_MAX 128 // 与内核 MAX_PATH_LEN 对齐

// 默认搜索路径，程序可用 set_path() 覆盖。
// 结尾的 '/' 覆盖“用户程序被放在根目录”的当前布局。
static const char *default_path = "/bin:/usr/bin:/";

const char *get_path(void)
{
        return default_path;
}

void set_path(const char *path)
{
        if (path != NULL)
        {
                default_path = path;
        }
}

static int has_slash(const char *s)
{
        for (; *s; s++)
        {
                if (*s == '/')
                {
                        return 1;
                }
        }
        return 0;
}

// 在 PATH 的每个目录里尝试 exec "dir/file"。
// exec 成功不会返回（进程已被替换）；只有当所有候选都失败时才返回 -1。
int execvp(const char *file, char **argv)
{
        const char *path;

        if (file == NULL || file[0] == '\0')
        {
                return -1;
        }

        // 含 '/' 视为显式路径，不搜索（POSIX 语义）
        if (has_slash(file))
        {
                return exec(file, argv);
        }

        path = default_path;
        for (;;)
        {
                const char *seg = path;
                const char *sep = path;
                size_t dirlen;
                size_t filelen;

                // 取下一个 ':'（或字符串结尾）
                while (*sep && *sep != ':')
                {
                        sep++;
                }
                dirlen = (size_t)(sep - seg);
                filelen = strlen(file);

                int ret = -1;
                if (dirlen == 0)
                {
                        // 空段（如 ":/bin" 或 "a::b"）按 POSIX 表示当前目录：
                        // 直接用裸文件名，由内核按 cwd 解析。
                        ret = exec(file, argv);
                }
                else if (dirlen + 1 + filelen + 1 <= EXEC_PATH_MAX)
                {
                        // 目录 + '/' + 文件名 + '\0' 必须放得下，否则跳过该候选。
                        char candidate[EXEC_PATH_MAX];

                        memcpy(candidate, seg, dirlen);
                        candidate[dirlen] = '/';
                        memcpy(candidate + dirlen + 1, file, filelen + 1);

                        ret = exec(candidate, argv);
                }
                // 成功不会走到这里；能到这只能是失败，继续下一个候选
                if (ret != -1)
                {
                        return ret;
                }

                if (*sep == '\0')
                {
                        break;
                }
                path = sep + 1;
        }

        return -1;
}