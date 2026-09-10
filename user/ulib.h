#ifndef _INC_USER_ULIB
#define _INC_USER_ULIB

#include <stdarg.h>

///////////////////////////////MACROS///////////////////////////////
#define EXIT_SUCCESS (0)
#define EXIT_FAILURE (1)
#define EOF (-1)
///////////////////////////////MACROS END///////////////////////////////

///////////////////////////////SYSCALLS///////////////////////////////
#define SYSCALL_FORK 1
#define SYSCALL_EXEC 2
#define SYSCALL_READ 3
#define SYSCALL_WRITE 4
#define SYSCALL_OPEN 5
#define SYSCALL_CLOSE 6
#define SYSCALL_STAT 7
#define SYSCALL_LSTAT 8
#define SYSCALL_FSTAT 9
#define SYSCALL_MMAP 10
#define SYSCALL_MUNMAP 11
#define SYSCALL_BRK 12
#define SYSCALL_SIGACTION 13
#define SYSCALL_SIGPROCMASK 14
#define SYSCALL_SIGRETURN 15
#define SYSCALL_IOCTL 16
#define SYSCALL_PREAD64 17
#define SYSCALL_PWRITE64 18
#define SYSCALL_READV 19
#define SYSCALL_WRITEV 20
#define SYSCALL_ACCESS 21
#define SYSCALL_PIPE 22
#define SYSCALL_SELECT 23
#define SYSCALL_SCHED_YIELD 24
#define SYSCALL_MREMAP 25
#define SYSCALL_MSYNC 26
#define SYSCALL_MINCORE 27
#define SYSCALL_MADVISE 28
#define SYSCALL_SHMGET 29
#define SYSCALL_SHMAT 30
#define SYSCALL_SHMCTL 31
#define SYSCALL_DUP 32
#define SYSCALL_DUP2 33
#define SYSCALL_PAUSE 34
#define SYSCALL_NANOSLEEP 35
#define SYSCALL_GETITIMER 36
#define SYSCALL_ALARM 37
#define SYSCALL_SETITIMER 38
#define SYSCALL_GETPID 39
#define SYSCALL_SENDFILE 40
#define SYSCALL_SOCKET 41
#define SYSCALL_CONNECT 42
#define SYSCALL_ACCEPT 43
#define SYSCALL_SENDTO 44
#define SYSCALL_RECVFROM 45
#define SYSCALL_SENDMSG 46
#define SYSCALL_RECVMSG 47
#define SYSCALL_SHUTDOWN 48
#define SYSCALL_BIND 49
#define SYSCALL_LISTEN 50
#define SYSCALL_GETSOCKNAME 51
#define SYSCALL_GETPEERNAME 52
#define SYSCALL_SOCKETPAIR 53
#define SYSCALL_SETSOCKOPT 54
#define SYSCALL_GETSOCKOPT 55
#define SYSCALL_EXIT 56
#define SYSCALL_GETPPID 57
#define SYSCALL_WAIT 58
#define SYSCALL_CHDIR 59
///////////////////////////////SYSCALLS END///////////////////////////////

///////////////////////////////TYPE DEFINITIONS///////////////////////////////
#define NULL (void *)0

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long int64_t;

typedef unsigned long size_t;
typedef void *voidp_t;
typedef int pid_t;
///////////////////////////////TYPE DEFINITIONS END///////////////////////////////

///////////////////////////////LIBRARY FUNCTIONS///////////////////////////////
#ifndef __LIB_H__
#define __LIB_H__
#define MAX(a, b) a > b ? a : b
#define MIN(a, b) a < b ? a : b

static inline char *strcpy_with_terminate(char *s, const char *t, int n)
{
        char *os;

        os = s;
        if (n <= 0)
                return os;
        while (--n > 0 && (*s++ = *t++) != 0)
                ;
        *s = 0;
        return os;
}

static inline void *memmove(void *dst, const void *src, uint32_t n)
{
        const char *s;
        char *d;

        if (n == 0)
                return dst;

        s = src;
        d = dst;
        if (s < d && s + n > d)
        {
                s += n;
                d += n;
                while (n-- > 0)
                        *--d = *--s;
        }
        else
                while (n-- > 0)
                        *d++ = *s++;

        return dst;
}

/**
 * memset - 将内存区域填满指定字符 (RISC-V)
 * @ptr:  起始地址
 * @value: 填充字符（按 unsigned char 处理）
 * @num:  字节数
 */
static inline void *memset(void *ptr, int value, size_t num)
{
        unsigned char *p = (unsigned char *)ptr;
        unsigned char val = (unsigned char)value;

        for (size_t i = 0; i < num; i++)
                p[i] = val;

        return ptr;
}

static inline int memcmp(const void *s1, const void *s2, size_t n)
{
        int ret;
        __asm__ volatile(
            "beqz   %2, 2f\n"
            "1:\n"
            "lbu    t0, 0(%0)\n"
            "lbu    t1, 0(%1)\n"
            "bne    t0, t1, 3f\n"
            "addi   %0, %0, 1\n"
            "addi   %1, %1, 1\n"
            "addi   %2, %2, -1\n"
            "bnez   %2, 1b\n"
            "2:\n"
            "li     %0, 0\n"
            "j      4f\n"
            "3:\n"
            "sub    %0, t0, t1\n"
            "4:\n"
            : "+r"(s1), "+r"(s2), "+r"(n)
            :
            : "t0", "t1", "memory");
        return ret;
}

/**
 * memcpy - 拷贝内存区域（不允许重叠）(RISC-V)
 * @dest:  目标地址
 * @src:   源地址
 * @num:   字节数
 */
static inline void *memcpy(void *dest, const void *src, size_t num)
{
        unsigned char *d = (unsigned char *)dest;
        const unsigned char *s = (const unsigned char *)src;

        for (size_t i = 0; i < num; i++)
                d[i] = s[i];

        return dest;
}

/**
 * strlen - 返回字符串长度（不含结尾 '\0'）
 * @s: 以 '\0' 结尾的字符串
 */
static inline size_t strlen(const char *s)
{
        const char *p = s;
        while (*p)
                p++;
        return (size_t)(p - s);
}
/**
 * strcmp - 比较两个字符串
 * @s1, @s2: 以 '\0' 结尾的字符串
 * 返回：0 相等，<0 s1 < s2，>0 s1 > s2
 */
static inline int strcmp(const char *s1, const char *s2)
{
        while (*s1 && (*s1 == *s2))
        {
                s1++;
                s2++;
        }
        return (unsigned char)*s1 - (unsigned char)*s2;
}
/**
 * strncmp - 比较前 n 个字符
 * @s1, @s2: 字符串
 * @n: 最多比较的字符数
 */
static inline int strncmp(const char *s1, const char *s2, size_t n)
{
        while (n-- && *s1 && (*s1 == *s2))
        {
                s1++;
                s2++;
        }
        if (n == (size_t)-1) // n 递减后溢出，表示 n 初始为 0
                return 0;
        return (unsigned char)*s1 - (unsigned char)*s2;
}
/**
 * strcpy - 将 src 复制到 dest（包括 '\0'）
 * @dest: 目标缓冲区（必须足够大）
 * @src: 源字符串
 * 返回 dest
 */
static inline char *strcpy(char *dest, const char *src)
{
        char *p = dest;
        while ((*p++ = *src++))
                ;
        return dest;
}
/**
 * strncpy - 复制最多 n 个字符，若 src 长度 < n 则用 '\0' 填充
 * @dest: 目标缓冲区
 * @src: 源字符串
 * @n: 最大复制字符数
 * 返回 dest
 */
static inline char *strncpy(char *dest, const char *src, size_t n)
{
        size_t i;
        for (i = 0; i < n && src[i]; i++)
                dest[i] = src[i];
        for (; i < n; i++)
                dest[i] = '\0';
        return dest;
}
/**
 * strcat - 将 src 追加到 dest 末尾
 * @dest: 目标字符串（已有 '\0'）
 * @src: 源字符串
 * 返回 dest
 */
static inline char *strcat(char *dest, const char *src)
{
        char *p = dest;
        while (*p)
                p++;            // 找到 dest 结尾
        while ((*p++ = *src++)) // 复制 src
                ;
        return dest;
}
/**
 * strncat - 追加最多 n 个字符，并加 '\0'
 * @dest: 目标字符串
 * @src: 源字符串
 * @n: 最大追加字符数
 * 返回 dest
 */
static inline char *strncat(char *dest, const char *src, size_t n)
{
        char *p = dest;
        size_t i;
        while (*p)
                p++;
        for (i = 0; i < n && src[i]; i++)
                p[i] = src[i];
        p[i] = '\0';
        return dest;
}

/**
 * strchr - 在字符串中查找字符第一次出现的位置
 * @s: 字符串
 * @c: 要查找的字符（会先转为 char 比较）
 * 返回：找到则返回指向该字符的指针，否则 NULL
 */
static inline char *strchr(const char *s, int c)
{
        char ch = (char)c;
        while (*s && *s != ch)
                s++;
        if (*s == ch)
                return (char *)s;
        return NULL;
}

/* 将字符串按分隔符切分为若干 token
 * str:   首次传待分割字符串，后续传 NULL
 * delim: 分隔符集合，任意一个字符都算分隔符
 * 返回:  下一个 token 的指针，没有更多时返回 NULL
 * 注意:  会修改原字符串，且不可重入 */
static inline char *strtok(char *str, const char *delim)
{
        static char *last = NULL;
        char *start;

        if (str != NULL)
                last = str;

        if (last == NULL || *last == '\0')
                return NULL;

        while (*last && strchr(delim, *last))
                last++;

        if (*last == '\0')
                return NULL;

        start = last;

        while (*last && !strchr(delim, *last))
                last++;

        if (*last)
        {
                *last = '\0';
                last++;
        }

        return start;
}

/**
 * strrchr - 在字符串中查找字符最后一次出现的位置
 * @s: 字符串
 * @c: 要查找的字符
 * 返回：指向最后出现位置的指针，或 NULL
 */
static inline char *strrchr(const char *s, int c)
{
        char ch = (char)c;
        const char *last = NULL;
        while (*s)
        {
                if (*s == ch)
                        last = s;
                s++;
        }
        // 注意：如果 c == '\0'，则 last 会被设置为指向结尾 '\0'
        // 这是标准行为，所以需要额外判断：
        if (ch == '\0')
                return (char *)s;
        return (char *)last;
}
/**
 * strstr - 子串查找
 * @haystack: 主字符串
 * @needle: 要查找的子串
 * 返回：指向第一次出现的位置，或 NULL
 */
static inline char *strstr(const char *haystack, const char *needle)
{
        if (!*needle)
                return (char *)haystack;
        while (*haystack)
        {
                const char *h = haystack;
                const char *n = needle;
                while (*h && *n && (*h == *n))
                {
                        h++;
                        n++;
                }
                if (!*n)
                        return (char *)haystack;
                haystack++;
        }
        return NULL;
}

static inline int strcasecmp(const char *s1, const char *s2)
{
        while (*s1 && *s2)
        {
                char c1 = *s1;
                char c2 = *s2;

                // 转大写
                if (c1 >= 'a' && c1 <= 'z')
                        c1 -= 'a' - 'A';
                if (c2 >= 'a' && c2 <= 'z')
                        c2 -= 'a' - 'A';

                if (c1 != c2)
                        return c1 - c2;
                s1++;
                s2++;
        }
        return *s1 - *s2;
}
#endif
///////////////////////////////LIBRARY FUNCTIONS END///////////////////////////////

int write(int fd, void *buf, uint64_t count);
int fork();
int exec(const char *path, char **argv);
int get_pid();
int get_ppid();
int wait(int *status);
int exit(int exit_code);
int printf(const char *fmt, ...);
int read(int fd, void *buf, uint64_t count);
int getchar(void);
int chdir(const char *path);
#endif