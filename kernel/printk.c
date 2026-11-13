/*
 * 这是给内核使用的printk
 * 任何系统调用都不应使用 printk
 *
 * 双通道输出：
 *   串口 to 纯文本，宿主机日志通道，不带转义码便于 grep；
 *   屏幕 to gpu_tty 控制台，颜色转义序列由 printk 自己维护：
 *           msg_fg 生效期间每条消息包裹 \033[<n>m ... \033[0m，
 *           串口侧不受影响。
 *           由于开机早期 gpu_tty 未就绪 则自动退化为仅串口。
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <lib.h>
#include <sbi.h>
#include <printk.h>
#include <type.h>
#include <spinlock.h>
#include <tty/gpu_tty.h>

static spinlock_t printing_lock = {0};

/* 当前 printk 屏幕侧前景色 SGR 码，-1 = 默认不包裹序列 */
static int msg_fg = -1;

#define is_digit(c) ((c) >= '0' && (c) <= '9')

static char *format_float(char *str, double num, int field_width, int precision, int flags);

int vsprintf(char *buf, const char *fmt, va_list args)
{
        char *str, *s;
        int flags;
        int field_width;
        int precision;
        int len, i;

        int qualifier; /* 'h'(short) 'H'(char=hh) 'l'(long) 'L'(ll) 'Z'(size_t) */

        for (str = buf; *fmt; fmt++)
        {

                if (*fmt != '%')
                {
                        *str++ = *fmt;
                        continue;
                }
                flags = 0;
        repeat:
                fmt++;
                switch (*fmt)
                {
                case '-':
                        flags |= LEFT;
                        goto repeat;
                case '+':
                        flags |= PLUS;
                        goto repeat;
                case ' ':
                        flags |= SPACE;
                        goto repeat;
                case '#':
                        flags |= SPECIAL;
                        goto repeat;
                case '0':
                        flags |= ZEROPAD;
                        goto repeat;
                }

                /* get field width */

                field_width = -1;
                if (is_digit(*fmt))
                        field_width = skip_atoi(&fmt);
                else if (*fmt == '*')
                {
                        fmt++;
                        field_width = va_arg(args, int);
                        if (field_width < 0)
                        {
                                field_width = -field_width;
                                flags |= LEFT;
                        }
                }

                /* get the precision */

                precision = -1;
                if (*fmt == '.')
                {
                        fmt++;
                        if (is_digit(*fmt))
                                precision = skip_atoi(&fmt);
                        else if (*fmt == '*')
                        {
                                fmt++;
                                precision = va_arg(args, int);
                        }
                        if (precision < 0)
                                precision = 0;
                }

                qualifier = -1;
                if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' || *fmt == 'Z')
                {
                        qualifier = *fmt;
                        fmt++;
                        /* 支持 ll / hh：复用 'L' 表示 long long，'H' 表示 char */
                        if (qualifier == 'l' && *fmt == 'l')
                        {
                                qualifier = 'L';
                                fmt++;
                        }
                        else if (qualifier == 'h' && *fmt == 'h')
                        {
                                qualifier = 'H';
                                fmt++;
                        }
                }

                switch (*fmt)
                {
                case 'c':

                        if (!(flags & LEFT))
                                while (--field_width > 0)
                                        *str++ = ' ';
                        *str++ = (unsigned char)va_arg(args, int);
                        while (--field_width > 0)
                                *str++ = ' ';
                        break;

                case 's':

                        s = va_arg(args, char *);
                        if (!s)
                                s = "(null)"; /* 修复：原 s='\0'(NULL) 会导致 strlen(NULL) 崩溃 */
                        len = strlen(s);
                        if (precision < 0)
                                precision = len;
                        else if (len > precision)
                                len = precision;

                        if (!(flags & LEFT))
                                while (len < field_width--)
                                        *str++ = ' ';
                        for (i = 0; i < len; i++)
                                *str++ = *s++;
                        while (len < field_width--)
                                *str++ = ' ';
                        break;

                case 'o':
                {
                        unsigned long long num;
                        if (qualifier == 'L')
                                num = va_arg(args, unsigned long long);
                        else if (qualifier == 'l')
                                num = va_arg(args, unsigned long);
                        else if (qualifier == 'h')
                                num = (unsigned short)va_arg(args, unsigned int);
                        else if (qualifier == 'H')
                                num = (unsigned char)va_arg(args, unsigned int);
                        else
                                num = va_arg(args, unsigned int);
                        str = number(str, num, 8, field_width, precision, flags, 0);
                        break;
                }

                case 'p':

                        if (field_width == -1)
                        {
                                field_width = 2 * sizeof(void *);
                                flags |= ZEROPAD;
                        }

                        str = number(str,
                                     (unsigned long long)(unsigned long)va_arg(args, void *),
                                     16,
                                     field_width,
                                     precision,
                                     flags,
                                     0);
                        break;

                case 'x':

                        flags |= SMALL;

                case 'X':
                {
                        unsigned long long num;
                        if (qualifier == 'L')
                                num = va_arg(args, unsigned long long);
                        else if (qualifier == 'l')
                                num = va_arg(args, unsigned long);
                        else if (qualifier == 'h')
                                num = (unsigned short)va_arg(args, unsigned int);
                        else if (qualifier == 'H')
                                num = (unsigned char)va_arg(args, unsigned int);
                        else
                                num = va_arg(args, unsigned int);
                        str = number(str, num, 16, field_width, precision, flags, 0);
                        break;
                }

                case 'd':
                case 'i':

                        flags |= SIGN;
                case 'u':
                {
                        unsigned long long num;
                        char sign = 0;
                        if (flags & SIGN)
                        {
                                long long v;
                                if (qualifier == 'L')
                                        v = va_arg(args, long long);
                                else if (qualifier == 'l')
                                        v = va_arg(args, long);
                                else if (qualifier == 'h')
                                        v = (short)va_arg(args, int);
                                else if (qualifier == 'H')
                                        v = (signed char)va_arg(args, int);
                                else
                                        v = va_arg(args, int);
                                if (v < 0)
                                {
                                        sign = '-';
                                        /* 用无符号取负避免 LLONG_MIN 溢出 */
                                        num = -(unsigned long long)v;
                                }
                                else
                                {
                                        num = (unsigned long long)v;
                                }
                        }
                        else
                        {
                                if (qualifier == 'L')
                                        num = va_arg(args, unsigned long long);
                                else if (qualifier == 'l')
                                        num = va_arg(args, unsigned long);
                                else if (qualifier == 'h')
                                        num = (unsigned short)va_arg(args, unsigned int);
                                else if (qualifier == 'H')
                                        num = (unsigned char)va_arg(args, unsigned int);
                                else
                                        num = va_arg(args, unsigned int);
                        }
                        str = number(str, num, 10, field_width, precision, flags, sign);
                        break;
                }

                case 'n':

                        if (qualifier == 'L')
                        {
                                long long *ip = va_arg(args, long long *);
                                *ip = (str - buf);
                        }
                        else if (qualifier == 'l')
                        {
                                long *ip = va_arg(args, long *);
                                *ip = (str - buf);
                        }
                        else
                        {
                                int *ip = va_arg(args, int *);
                                *ip = (str - buf);
                        }
                        break;

                case 'f':
                        flags |= SMALL; /* %f 输出小写 nan/inf */
                case 'F':
                {
                        double num = va_arg(args, double);
                        str = format_float(str, num, field_width, precision, flags);
                        break;
                }

                case '%':

                        *str++ = '%';
                        break;

                default:

                        *str++ = '%';
                        if (*fmt)
                                *str++ = *fmt;
                        else
                                fmt--;
                        break;
                }
        }
        *str = '\0';
        return str - buf;
}

/*
 * 控制台发射：格式化好的消息同时发串口和屏幕。
 * 串口始终纯文本；屏幕侧在 msg_fg 生效时由 printk 自己拼装
 * \033[<n>m ... \033[0m 包裹（gpu_tty 未就绪时 gpu_tty_console_write
 * 返回 -1，自动退化为仅串口。
 */
int printk_screen_ready(void)
{
        return gpu_tty_console_write("", 0) == 0;
}

static void printk_emit(const char *s, int len)
{
        int i;

        /* 串口（SBI 控制台）：纯文本 */
        for (i = 0; i < len; i++)
        	sbi_putchar(s[i]);

        /* 屏幕gpu_tty：文本只写一次；msg_fg 生效时由 printk 自己
         * 拼装 \033[<n>m ... \033[0m 包裹，未就绪则整体退化仅串口 */
        if (!printk_screen_ready())
                return;

        if (msg_fg >= 0)
        {
                char seq[16];
                int n = 0, v = msg_fg;
                char tmp[4];

                /* 进色 \033[<n>m */
                seq[n++] = '\033';
                seq[n++] = '[';
                int m = 0;
                if (v == 0)
                {
                        tmp[m++] = '0';
                }
                else
                {
                        while (v)
                        {
                                tmp[m++] = '0' + v % 10;
                                v /= 10;
                        }
                }
                while (m)
                        seq[n++] = tmp[--m];
                seq[n++] = 'm';
                gpu_tty_console_write(seq, n);
        }

        gpu_tty_console_write(s, len);

        if (msg_fg >= 0)
                gpu_tty_console_write("\033[0m", 4); /* 复位 */
}

void printk(char *fmt, ...)
{

        va_list args;
        char buf[PRINT_BUFFER_SIZE];
        va_start(args, fmt);
        vsprintf(buf, fmt, args);
        va_end(args);

        acquire(&printing_lock);
        printk_emit(buf, strlen(buf));
        release(&printing_lock);
}

/*
 * 带颜色的内核打印：fg_sgr 为 SGR 前景色码（31=红、32=绿、33=黄…），
 * 只影响本条消息的屏幕侧显示。供 panic 等需要醒目提示的路径使用。
 */
void printk_color(int fg_sgr, char *fmt, ...)
{
        va_list args;
        char buf[PRINT_BUFFER_SIZE];
        int saved;

        va_start(args, fmt);
        vsprintf(buf, fmt, args);
        va_end(args);

        acquire(&printing_lock);
        saved = msg_fg;
        msg_fg = fg_sgr;
        printk_emit(buf, strlen(buf));
        msg_fg = saved;
        release(&printing_lock);
}

int skip_atoi(const char **s)
{
        int i = 0;

        while (is_digit(**s))
                i = i * 10 + *((*s)++) - '0';
        return i;
}

// do_div
static int do_div(unsigned long long *n, int base)
{
        unsigned long long quotient = *n / base;
        int remainder = (int)(*n - quotient * base); // 等价于 %
        *n = quotient;
        return remainder;
}

/*
 * 数值格式化。
 * num:   待打印数值的绝对值（无符号 64 位，避免有符号取负溢出）
 * sign:  '-' 表示负数，0 表示非负（此时再由 PLUS/SPACE 决定是否补符号）
 */
static char *
number(char *str, unsigned long long num, int base, int size, int precision, int type, char sign)
{
        char c, tmp[50];
        const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        int i;

        if (type & SMALL)
                digits = "0123456789abcdefghijklmnopqrstuvwxyz";
        if (type & LEFT)
                type &= ~ZEROPAD;
        if (base < 2 || base > 36)
                return 0;

        c = (type & ZEROPAD) ? '0' : ' ';

        /* 负数符号由调用方传入；非负数根据 +/空格标志补符号 */
        if (!sign)
                sign = (type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);

        if (sign)
                size--;
        if (type & SPECIAL)
        {
                if (base == 16)
                        size -= 2;
                else if (base == 8)
                        size--;
        }

        i = 0;
        if (num == 0)
        {
                tmp[i++] = '0';
        }
        else
        {
                unsigned long long n = num;
                while (n != 0)
                {
                        tmp[i++] = digits[do_div(&n, base)];
                }
        }

        if (i > precision)
                precision = i;
        size -= precision;

        if (!(type & (ZEROPAD + LEFT)))
        {
                while (size-- > 0)
                        *str++ = ' ';
        }

        if (sign)
                *str++ = sign;

        if (type & SPECIAL)
        {
                if (base == 8)
                        *str++ = '0';
                else if (base == 16)
                {
                        *str++ = '0';
                        *str++ = digits[33];
                }
        }

        if (!(type & LEFT))
        {
                while (size-- > 0)
                        *str++ = c;
        }

        while (i < precision--)
                *str++ = '0';
        while (i-- > 0)
                *str++ = tmp[i];
        while (size-- > 0)
                *str++ = ' ';

        return str;
}

/*
 * 浮点数格式化 %f / %F。
 * 支持：精度（默认 6）、字段宽度、左对齐、零填充、+/-/空格 符号、
 *       # 强制小数点、NaN / Inf。
 * 整数与小数部分均用整数运算输出，仅取参和拆分时使用 FPU。
 */
static char *
format_float(char *str, double num, int field_width, int precision, int flags)
{
        char tmp[128];
        char *p = tmp;
        char sign = 0;
        int len, i;
        int is_nan = 0, is_inf = 0;

        if (precision < 0)
                precision = 6;
        /* 限制精度上限：double 尾数 52 位，仅 2^53≈9e15 以内的整数可被
         * 精确表示，故 scale=10^precision 中 precision 超过 15 时
         * frac*scale 会丢位导致末几位偏差。内核调试场景 15 位已足够。 */
        if (precision > 15)
                precision = 15;

        /* 通过 IEEE 754 位模式检测 NaN / Inf */
        {
                union
                {
                        double d;
                        uint64_t u;
                } conv;
                conv.d = num;
                uint64_t bits = conv.u;
                int exp = (int)((bits >> 52) & 0x7ff);
                uint64_t mant = bits & 0xfffffffffffffULL;

                if (exp == 0x7ff)
                {
                        if (mant == 0)
                                is_inf = 1;
                        else
                                is_nan = 1;
                }
        }

        if (is_nan)
        {
                const char *s = (flags & SMALL) ? "nan" : "NAN";
                if (sign)
                        *p++ = sign;
                for (i = 0; s[i]; i++)
                        *p++ = s[i];
                len = p - tmp;
                if (!(flags & LEFT))
                        while (len < field_width--)
                                *str++ = ' ';
                for (i = 0; i < len; i++)
                        *str++ = tmp[i];
                while (len < field_width--)
                        *str++ = ' ';
                return str;
        }

        if (is_inf)
        {
                const char *s;
                if (num < 0)
                {
                        sign = '-';
                        s = (flags & SMALL) ? "inf" : "INF";
                }
                else
                {
                        s = (flags & SMALL) ? "inf" : "INF";
                        if (flags & PLUS)
                                sign = '+';
                        else if (flags & SPACE)
                                sign = ' ';
                }
                if (sign)
                        *p++ = sign;
                for (i = 0; s[i]; i++)
                        *p++ = s[i];
                len = p - tmp;
                if (!(flags & LEFT))
                        while (len < field_width--)
                                *str++ = ' ';
                for (i = 0; i < len; i++)
                        *str++ = tmp[i];
                while (len < field_width--)
                        *str++ = ' ';
                return str;
        }

        /* 符号 */
        if (num < 0)
        {
                sign = '-';
                num = -num;
        }
        else if (flags & PLUS)
        {
                sign = '+';
        }
        else if (flags & SPACE)
        {
                sign = ' ';
        }

        /* 拆分整数部分与小数部分 */
        unsigned long long int_part = (unsigned long long)num;
        double frac = num - (double)int_part;

        /* 计算 10^precision，用于把小数放大成整数 */
        unsigned long long scale = 1;
        for (i = 0; i < precision; i++)
                scale *= 10;

        /* 四舍五入 */
        unsigned long long frac_int = (unsigned long long)(frac * scale + 0.5);

        /* 进位到整数部分 */
        if (frac_int >= scale)
        {
                int_part++;
                frac_int -= scale;
        }

        /* 数字部分（不含符号）写入 tmp */
        char ibuf[32];
        int ilen = 0;
        unsigned long long ip = int_part;
        if (ip == 0)
        {
                ibuf[ilen++] = '0';
        }
        else
        {
                while (ip > 0)
                {
                        ibuf[ilen++] = '0' + (ip % 10);
                        ip /= 10;
                }
        }
        for (i = ilen - 1; i >= 0; i--)
                *p++ = ibuf[i];

        /* 小数点 */
        if (precision > 0 || (flags & SPECIAL))
                *p++ = '.';

        /* 小数部分：补足 precision 位（前导零），倒序输出 */
        if (precision > 0)
        {
                char fbuf[32];
                int flen = 0;
                unsigned long long f = frac_int;
                if (f == 0)
                {
                        fbuf[flen++] = '0';
                }
                else
                {
                        while (f > 0)
                        {
                                fbuf[flen++] = '0' + (f % 10);
                                f /= 10;
                        }
                }
                /* 前导补零到 precision 位 */
                while (flen < precision)
                        fbuf[flen++] = '0';
                /* 只取 precision 位，倒序写出 */
                for (i = precision - 1; i >= 0; i--)
                        *p++ = fbuf[i];
        }

        len = p - tmp;

        /* 总宽度需扣除符号所占的 1 字节 */
        int sign_len = sign ? 1 : 0;

        if (!(flags & LEFT))
        {
                if ((flags & ZEROPAD))
                {
                        /* 零填充：先输出符号，再补零，再输出数字 */
                        if (sign)
                                *str++ = sign;
                        while (len + sign_len < field_width--)
                                *str++ = '0';
                }
                else
                {
                        /* 空格填充：先补空格，再输出符号+数字 */
                        while (len + sign_len < field_width--)
                                *str++ = ' ';
                        if (sign)
                                *str++ = sign;
                }
        }
        else
        {
                /* 左对齐：先符号+数字，再补空格 */
                if (sign)
                        *str++ = sign;
        }
        for (i = 0; i < len; i++)
                *str++ = tmp[i];
        while (len + sign_len < field_width--)
                *str++ = ' ';

        return str;
}
