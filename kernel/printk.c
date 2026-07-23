#include "../include/stdarg.h"
#include "../include/stddef.h"
#include "../include/stdint.h"
#include "../include/lib.h"
#include "sbi.h"
#include "printk.h"

#define is_digit(c) ((c) >= '0' && (c) <= '9')

// 辅助：将整数写入 num 缓冲区，返回字符数
// base: 进制（8,10,16），uppercase 仅对十六进制有效
// sign: 0 无符号，1 有符号（正号、空格在外部处理）
static int format_number(char *num, unsigned long long value, int base,
                         int uppercase, int sign, int negative)
{
        char digits[] = "0123456789abcdef";
        char *p = num + 64; // 从末尾开始写
        *p = '\0';
        int len = 0;

        if (value == 0)
        {
                *--p = '0';
                len = 1;
        }
        else
        {
                while (value)
                {
                        int d = value % base;
                        char c = digits[d];
                        if (uppercase && c >= 'a' && c <= 'f')
                                c -= 32;
                        *--p = c;
                        value /= base;
                        len++;
                }
        }
        int i;
        for (i = 0; i < len; i++)
                num[i] = p[i];
        num[len] = '\0';
        return len;
}

// 主函数：格式化输出到 buf
int vsprintf(char *buf, const char *fmt, va_list args)
{
        char *str, *s;
        int flags;
        int field_width;
        int precision;
        int len, i;

        int qualifier; /* 'h', 'l', 'L' or 'Z' for integer fields */

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
                                s = '\0';
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

                        if (qualifier == 'l')
                                str = number(str, va_arg(args, unsigned long), 8, field_width, precision, flags);
                        else
                                str = number(str, va_arg(args, unsigned int), 8, field_width, precision, flags);
                        break;

                case 'p':

                        if (field_width == -1)
                        {
                                field_width = 2 * sizeof(void *);
                                flags |= ZEROPAD;
                        }

                        str = number(str, (unsigned long)va_arg(args, void *), 16, field_width, precision, flags);
                        break;

                case 'x':

                        flags |= SMALL;

                case 'X':

                        if (qualifier == 'l')
                                str = number(str, va_arg(args, unsigned long), 16, field_width, precision, flags);
                        else
                                str = number(str, va_arg(args, unsigned int), 16, field_width, precision, flags);
                        break;

                case 'd':
                case 'i':

                        flags |= SIGN;
                case 'u':

                        if (qualifier == 'l')
                                str = number(str, va_arg(args, unsigned long), 10, field_width, precision, flags);
                        else
                                str = number(str, va_arg(args, unsigned int), 10, field_width, precision, flags);
                        break;

                case 'n':

                        if (qualifier == 'l')
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

void printk(char *fmt, ...)
{

        va_list args;
        char buf[PRINT_BUFFER_SIZE];
        char *p = buf;
        va_start(args, fmt);
        vsprintf(buf, fmt, args); // 将 va_list 传递给 vsprintf
        va_end(args);
        while (*p != '\0')
        {
                sbi_putchar(*p);
                p++;
        }
}

int skip_atoi(const char **s)
{
        int i = 0;

        while (is_digit(**s))
                i = i * 10 + *((*s)++) - '0';
        return i;
}
// do_div 的C语言版本
static int do_div(unsigned long long *n, int base)
{
        unsigned long long quotient = *n / base;
        int remainder = (int)(*n - quotient * base); // 等价于 %，但更快
        *n = quotient;
        return remainder;
}

// 原 number 函数（只改了 do_div 调用）
static char *number(char *str, long num, int base, int size, int precision, int type)
{
        char c, sign, tmp[50];
        const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        int i;

        if (type & SMALL)
                digits = "0123456789abcdefghijklmnopqrstuvwxyz";
        if (type & LEFT)
                type &= ~ZEROPAD;
        if (base < 2 || base > 36)
                return 0;

        c = (type & ZEROPAD) ? '0' : ' ';
        sign = 0;

        if (type & SIGN && num < 0)
        {
                sign = '-';
                num = -num;
        }
        else
        {
                sign = (type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);
        }

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
                unsigned long long n = (unsigned long long)num;
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
