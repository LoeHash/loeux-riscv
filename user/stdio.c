#include <stdio.h>

static char scanf_pushback;
static int scanf_has_pushback = 0;

static int scanf_getchar(char *c)
{
        if (scanf_has_pushback)
        {
                *c = scanf_pushback;
                scanf_has_pushback = 0;
                return 1;
        }

        return read(0, c, 1) == 1 ? 1 : EOF;
}

static void scanf_ungetchar(char c)
{
        scanf_pushback = c;
        scanf_has_pushback = 1;
}

static int scanf_is_space(char c)
{
        return c == ' ' ||
               c == '\t' ||
               c == '\n' ||
               c == '\r' ||
               c == '\v' ||
               c == '\f';
}

static void scanf_skip_space(void)
{
        char c;

        while (scanf_getchar(&c) != EOF)
        {
                if (!scanf_is_space(c))
                {
                        scanf_ungetchar(c);
                        return;
                }
        }
}

static int scanf_digit(char c, int base)
{
        int value;

        if (c >= '0' && c <= '9')
        {
                value = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
                value = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
                value = c - 'A' + 10;
        }
        else
        {
                return -1;
        }

        if (value >= base)
        {
                return -1;
        }

        return value;
}

static int scanf_read_integer(const char *fmt, void *value)
{
        char c;
        int sign = 1;
        int base = 10;
        int digit;
        int found = 0;
        unsigned long long number = 0;

        scanf_skip_space();

        if (scanf_getchar(&c) == EOF)
        {
                return EOF;
        }

        if (c == '-')
        {
                sign = -1;

                if (scanf_getchar(&c) == EOF)
                {
                        return 0;
                }
        }
        else if (c == '+')
        {
                if (scanf_getchar(&c) == EOF)
                {
                        return 0;
                }
        }

        if (*fmt == 'x')
        {
                base = 16;

                if (c == '0')
                {
                        char next;

                        if (scanf_getchar(&next) != EOF)
                        {
                                if (next == 'x' || next == 'X')
                                {
                                        if (scanf_getchar(&c) == EOF)
                                        {
                                                return 0;
                                        }
                                }
                                else
                                {
                                        scanf_ungetchar(next);
                                }
                        }
                }
        }

        while (1)
        {
                digit = scanf_digit(c, base);

                if (digit < 0)
                {
                        scanf_ungetchar(c);
                        break;
                }

                number = number * base + digit;
                found = 1;

                if (scanf_getchar(&c) == EOF)
                {
                        break;
                }
        }

        if (!found)
        {
                return 0;
        }

        if (*fmt == 'd')
        {
                int *result = (int *)value;
                *result = sign == -1 ? -(int)number : (int)number;
        }
        else if (*fmt == 'u')
        {
                unsigned int *result = (unsigned int *)value;
                *result = (unsigned int)number;
        }
        else
        {
                unsigned int *result = (unsigned int *)value;
                *result = (unsigned int)number;
        }

        return 1;
}

static int scanf_read_string(char *value)
{
        char c;
        int length = 0;

        scanf_skip_space();

        while (scanf_getchar(&c) != EOF)
        {
                if (scanf_is_space(c))
                {
                        scanf_ungetchar(c);
                        break;
                }

                value[length++] = c;
        }

        value[length] = '\0';

        return length > 0 ? 1 : EOF;
}

int scanf(const char *fmt, ...)
{
        va_list ap;
        int matched = 0;

        va_start(ap, fmt);

        while (*fmt)
        {
                if (scanf_is_space(*fmt))
                {
                        while (scanf_is_space(*fmt))
                        {
                                fmt++;
                        }

                        scanf_skip_space();
                        continue;
                }

                if (*fmt != '%')
                {
                        char c;

                        if (scanf_getchar(&c) == EOF)
                        {
                                break;
                        }

                        if (c != *fmt)
                        {
                                scanf_ungetchar(c);
                                break;
                        }

                        fmt++;
                        continue;
                }

                fmt++;

                if (*fmt == '%')
                {
                        char c;

                        if (scanf_getchar(&c) == EOF)
                        {
                                break;
                        }

                        if (c != '%')
                        {
                                scanf_ungetchar(c);
                                break;
                        }
                }
                else if (*fmt == 'd')
                {
                        int *value = va_arg(ap, int *);

                        int ret = scanf_read_integer(fmt, value);

                        if (ret == EOF)
                        {
                                if (matched == 0)
                                {
                                        matched = EOF;
                                }

                                break;
                        }

                        if (ret == 0)
                        {
                                break;
                        }

                        matched++;
                }
                else if (*fmt == 'u')
                {
                        unsigned int *value = va_arg(ap, unsigned int *);

                        int ret = scanf_read_integer(fmt, value);

                        if (ret == EOF)
                        {
                                if (matched == 0)
                                {
                                        matched = EOF;
                                }

                                break;
                        }

                        if (ret == 0)
                        {
                                break;
                        }

                        matched++;
                }
                else if (*fmt == 'x')
                {
                        unsigned int *value = va_arg(ap, unsigned int *);

                        int ret = scanf_read_integer(fmt, value);

                        if (ret == EOF)
                        {
                                if (matched == 0)
                                {
                                        matched = EOF;
                                }

                                break;
                        }

                        if (ret == 0)
                        {
                                break;
                        }

                        matched++;
                }
                else if (*fmt == 'c')
                {
                        char *value = va_arg(ap, char *);

                        if (scanf_getchar(value) == EOF)
                        {
                                if (matched == 0)
                                {
                                        matched = EOF;
                                }

                                break;
                        }

                        matched++;
                }
                else if (*fmt == 's')
                {
                        char *value = va_arg(ap, char *);

                        int ret = scanf_read_string(value);

                        if (ret == EOF)
                        {
                                if (matched == 0)
                                {
                                        matched = EOF;
                                }

                                break;
                        }

                        matched++;
                }

                fmt++;
        }

        va_end(ap);

        return matched;
}

int getchar(void)
{
        static char buf[256];
        static int pos = 0;
        static int len = 0;

        if (pos >= len)
        {
                int n = read(0, buf, 256);
                if (n <= 0)
                {
                        return -1; // EOF
                }
                len = n;
                pos = 0;
        }
        return (int)(unsigned char)buf[pos++];
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

// 数值已格式化为 src[0..len)，按 width / 0 填充 / 左对齐标志放入 out。
// prefix 用于 %x/%o 的 "#" 备选形式（如 0x）。
// 注意：0 填充不处理负数符号位（本内核 printf 场景里宽度只用于非负数）。
static void buf_append_num(char *out, int *pos, int cap,
                           const char *prefix, int prefix_len,
                           const char *src, int len,
                           int width, int zero_pad, int left_justify)
{
        int total = prefix_len + len;
        int i;

        if (!left_justify && width > total)
        {
                // 0 填充时前缀（0x）要在 0 的前面：前缀 → 0 → 数字；
                // 普通空格填充：空格 → 前缀 → 数字。
                if (zero_pad)
                {
                        buf_append(out, pos, cap, prefix, prefix_len);
                        prefix_len = 0;
                }
                char pad = zero_pad ? '0' : ' ';
                for (i = total; i < width; i++)
                        buf_append_char(out, pos, cap, pad);
        }
        buf_append(out, pos, cap, prefix, prefix_len);
        buf_append(out, pos, cap, src, len);
        if (left_justify && width > total)
        {
                for (i = total; i < width; i++)
                        buf_append_char(out, pos, cap, ' ');
        }
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
                                buf_append_num(out, &pos, PRINTF_BUF_SZ, "", 0,
                                               buf, len, width, zero_pad, left_justify);
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
                                buf_append_num(out, &pos, PRINTF_BUF_SZ, "", 0,
                                               buf, len, width, zero_pad, left_justify);
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
                                int len = uint_to_str(val, buf, 16, uppercase);
                                buf_append_num(out, &pos, PRINTF_BUF_SZ,
                                               (alt_form && val != 0) ? "0x" : "",
                                               (alt_form && val != 0) ? 2 : 0,
                                               buf, len, width, zero_pad, left_justify);
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
                                int len = uint_to_str(val, buf, 16, 1);
                                buf_append_num(out, &pos, PRINTF_BUF_SZ,
                                               (alt_form && val != 0) ? "0X" : "",
                                               (alt_form && val != 0) ? 2 : 0,
                                               buf, len, width, zero_pad, left_justify);
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
                                int len = uint_to_str(val, buf, 8, 0);
                                buf_append_num(out, &pos, PRINTF_BUF_SZ,
                                               (alt_form && val != 0) ? "0" : "",
                                               (alt_form && val != 0) ? 1 : 0,
                                               buf, len, width, zero_pad, left_justify);
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