#include "ulib.h"

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