#include "ulib.h"

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

// 往 stdout 写字符串
static void write_str(const char *str, int len)
{
        if (len <= 0)
                return;
        write(1, (void *)str, len);
}

// 往 stdout 写单个字符
static void write_char(char c)
{
        write(1, (void *)&c, 1);
}

// 计算字符串长度
static int my_strlen(const char *s)
{
        int len = 0;
        while (s[len])
                len++;
        return len;
}

// 无符号整数转字符串（支持 2-36 进制）
static int uint_to_str(unsigned long long num, char *buf, int base, int uppercase)
{
        const char *digits_lower = "0123456789abcdefghijklmnopqrstuvwxyz";
        const char *digits_upper = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const char *digits = uppercase ? digits_upper : digits_lower;

        char temp[65];
        int i = 0;

        if (num == 0)
        {
                temp[i++] = '0';
        }
        else
        {
                while (num > 0)
                {
                        temp[i++] = digits[num % base];
                        num /= base;
                }
        }

        // 反转
        for (int j = 0; j < i; j++)
        {
                buf[j] = temp[i - 1 - j];
        }
        buf[i] = '\0';
        return i;
}

// 有符号整数转字符串
static int int_to_str(long long num, char *buf)
{
        char temp[32];
        int i = 0;
        unsigned long long n;
        int negative = 0;

        if (num < 0)
        {
                negative = 1;
                n = -num;
        }
        else
        {
                n = num;
        }

        if (n == 0)
        {
                temp[i++] = '0';
        }
        else
        {
                while (n > 0)
                {
                        temp[i++] = '0' + (n % 10);
                        n /= 10;
                }
        }

        if (negative)
        {
                temp[i++] = '-';
        }

        for (int j = 0; j < i; j++)
        {
                buf[j] = temp[i - 1 - j];
        }
        buf[i] = '\0';
        return i;
}

// 指针转字符串（带 0x 前缀）
static int ptr_to_str(void *ptr, char *buf)
{
        buf[0] = '0';
        buf[1] = 'x';
        unsigned long long addr = (unsigned long long)ptr;
        int len = uint_to_str(addr, buf + 2, 16, 0);
        return len + 2;
}

// printf 内部缓冲区：把整个格式化结果攒到一起，
// 最后用一次 write 输出，这样 sleeplock 保护的是整条 printf，
// 避免 fork 后父子进程的输出在两次 write 之间交错。
#define PRINTF_BUF_SZ 512

// 往缓冲区追加一段数据
static void buf_append(char *out, int *pos, int cap, const char *src, int len)
{
        for (int i = 0; i < len && *pos < cap; i++)
                out[(*pos)++] = src[i];
}

// 往缓冲区追加一个字符
static void buf_append_char(char *out, int *pos, int cap, char c)
{
        if (*pos < cap)
                out[(*pos)++] = c;
}

int printf(const char *fmt, ...)
{
        va_list args;
        va_start(args, fmt);

        char buf[64];
        char out[PRINTF_BUF_SZ];
        int pos = 0;

        while (*fmt)
        {
                if (*fmt == '%')
                {
                        fmt++;

                        // 处理长度修饰符
                        int long_mod = 0; // 0=int, 1=long, 2=long long
                        int alt_form = 0; // # 标志
                        int uppercase = 0;
                        int width = 0;        // 宽度
                        int precision = -1;   // 精度（-1 表示默认）
                        int zero_pad = 0;     // 0 填充
                        int left_justify = 0; // 左对齐

                        // === 处理标志 ===
                        while (*fmt == '#' || *fmt == '0' || *fmt == '-' || *fmt == ' ')
                        {
                                switch (*fmt)
                                {
                                case '#':
                                        alt_form = 1;
                                        break;
                                case '0':
                                        zero_pad = 1;
                                        break;
                                case '-':
                                        left_justify = 1;
                                        break;
                                case ' ': /* 忽略空格标志 */
                                        break;
                                }
                                fmt++;
                        }

                        // === 处理宽度 ===
                        while (*fmt >= '0' && *fmt <= '9')
                        {
                                width = width * 10 + (*fmt - '0');
                                fmt++;
                        }

                        // === 处理精度 ===
                        if (*fmt == '.')
                        {
                                fmt++;
                                precision = 0;
                                while (*fmt >= '0' && *fmt <= '9')
                                {
                                        precision = precision * 10 + (*fmt - '0');
                                        fmt++;
                                }
                        }

                        // === 处理长度修饰符 ===
                        if (*fmt == 'l')
                        {
                                fmt++;
                                if (*fmt == 'l')
                                {
                                        long_mod = 2;
                                        fmt++;
                                }
                                else
                                {
                                        long_mod = 1;
                                }
                        }

                        // === 处理格式化字符 ===
                        switch (*fmt)
                        {
                        case 'd':
                        case 'i':
                        {
                                long long val;
                                if (long_mod == 2)
                                {
                                        val = va_arg(args, long long);
                                }
                                else if (long_mod == 1)
                                {
                                        val = va_arg(args, long);
                                }
                                else
                                {
                                        val = va_arg(args, int);
                                }
                                int len = int_to_str(val, buf);
                                buf_append(out, &pos, PRINTF_BUF_SZ, buf, len);
                                break;
                        }

                        case 'u':
                        {
                                unsigned long long val;
                                if (long_mod == 2)
                                {
                                        val = va_arg(args, unsigned long long);
                                }
                                else if (long_mod == 1)
                                {
                                        val = va_arg(args, unsigned long);
                                }
                                else
                                {
                                        val = va_arg(args, unsigned int);
                                }
                                int len = uint_to_str(val, buf, 10, 0);
                                buf_append(out, &pos, PRINTF_BUF_SZ, buf, len);
                                break;
                        }

                        case 'x':
                        {
                                unsigned long long val;
                                if (long_mod == 2)
                                {
                                        val = va_arg(args, unsigned long long);
                                }
                                else if (long_mod == 1)
                                {
                                        val = va_arg(args, unsigned long);
                                }
                                else
                                {
                                        val = va_arg(args, unsigned int);
                                }
                                if (alt_form && val != 0)
                                {
                                        buf_append(out, &pos, PRINTF_BUF_SZ, "0x", 2);
                                }
                                int len = uint_to_str(val, buf, 16, uppercase);
                                buf_append(out, &pos, PRINTF_BUF_SZ, buf, len);
                                break;
                        }

                        case 'X':
                        {
                                uppercase = 1;
                                unsigned long long val;
                                if (long_mod == 2)
                                {
                                        val = va_arg(args, unsigned long long);
                                }
                                else if (long_mod == 1)
                                {
                                        val = va_arg(args, unsigned long);
                                }
                                else
                                {
                                        val = va_arg(args, unsigned int);
                                }
                                if (alt_form && val != 0)
                                {
                                        buf_append(out, &pos, PRINTF_BUF_SZ, "0X", 2);
                                }
                                int len = uint_to_str(val, buf, 16, 1);
                                buf_append(out, &pos, PRINTF_BUF_SZ, buf, len);
                                break;
                        }

                        case 'o':
                        {
                                unsigned long long val;
                                if (long_mod == 2)
                                {
                                        val = va_arg(args, unsigned long long);
                                }
                                else if (long_mod == 1)
                                {
                                        val = va_arg(args, unsigned long);
                                }
                                else
                                {
                                        val = va_arg(args, unsigned int);
                                }
                                if (alt_form && val != 0)
                                {
                                        buf_append_char(out, &pos, PRINTF_BUF_SZ, '0');
                                }
                                int len = uint_to_str(val, buf, 8, 0);
                                buf_append(out, &pos, PRINTF_BUF_SZ, buf, len);
                                break;
                        }

                        case 'p':
                        {
                                void *ptr = va_arg(args, void *);
                                int len = ptr_to_str(ptr, buf);
                                buf_append(out, &pos, PRINTF_BUF_SZ, buf, len);
                                break;
                        }

                        case 's':
                        {
                                char *str = va_arg(args, char *);
                                if (str == NULL)
                                        str = "(null)";
                                int len = my_strlen(str);

                                // 处理精度（截断）
                                if (precision >= 0 && precision < len)
                                {
                                        len = precision;
                                }

                                // 处理宽度
                                if (width > 0 && !left_justify)
                                {
                                        char pad = zero_pad ? '0' : ' ';
                                        for (int i = len; i < width; i++)
                                        {
                                                buf_append_char(out, &pos, PRINTF_BUF_SZ, pad);
                                        }
                                }

                                buf_append(out, &pos, PRINTF_BUF_SZ, str, len);

                                if (width > 0 && left_justify)
                                {
                                        for (int i = len; i < width; i++)
                                        {
                                                buf_append_char(out, &pos, PRINTF_BUF_SZ, ' ');
                                        }
                                }
                                break;
                        }

                        case 'c':
                        {
                                char ch = (char)va_arg(args, int);
                                buf_append_char(out, &pos, PRINTF_BUF_SZ, ch);
                                break;
                        }

                        case '%':
                        {
                                buf_append_char(out, &pos, PRINTF_BUF_SZ, '%');
                                break;
                        }

                        default:
                                buf_append_char(out, &pos, PRINTF_BUF_SZ, '%');
                                buf_append_char(out, &pos, PRINTF_BUF_SZ, *fmt);
                                break;
                        }
                        fmt++;
                }
                else
                {
                        // 普通字符
                        buf_append_char(out, &pos, PRINTF_BUF_SZ, *fmt);
                        fmt++;
                }
        }

        va_end(args);

        // 一次性输出整个缓冲区，sleeplock 保护这一整条 printf
        if (pos > 0)
                write(1, out, pos);

        return pos;
}