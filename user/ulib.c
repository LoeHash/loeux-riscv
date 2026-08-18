#include "ulib.h"

int write(int fd, void *buf, uint64_t count)
{
        int ret;
        __asm__ volatile(
            "li a7, %4\n"
            "ecall\n"
            "mv %0, a0\n"
            : "=r"(ret)
            : "r"(fd), "r"(buf), "r"(count), "i"(SYSCALL_WRITE)
            : "a0", "a1", "a2", "a7", "memory");
        return ret;
}