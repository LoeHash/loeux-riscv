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

// 辅助：将整数写入 num 缓冲区，返回字符数
// base: 进制（8,10,16），uppercase 仅对十六进制有效
// sign: 0 无符号，1 有符号（正号、空格在外部处理）
static int format_number(char* num,
			 unsigned long long value,
			 int base,
			 int uppercase,
			 int sign,
			 int negative)
{
	char digits[] = "0123456789abcdef";
	char* p = num + 64; // 从末尾开始写
	*p = '\0';
	int len = 0;

	if (value == 0) {
		*--p = '0';
		len = 1;
	} else {
		while (value) {
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

int vsprintf(char* buf, const char* fmt, va_list args)
{
	char *str, *s;
	int flags;
	int field_width;
	int precision;
	int len, i;

	int qualifier; /* 'h', 'l', 'L' or 'Z' for integer fields */

	for (str = buf; *fmt; fmt++) {

		if (*fmt != '%') {
			*str++ = *fmt;
			continue;
		}
		flags = 0;
	repeat:
		fmt++;
		switch (*fmt) {
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
		else if (*fmt == '*') {
			fmt++;
			field_width = va_arg(args, int);
			if (field_width < 0) {
				field_width = -field_width;
				flags |= LEFT;
			}
		}

		/* get the precision */

		precision = -1;
		if (*fmt == '.') {
			fmt++;
			if (is_digit(*fmt))
				precision = skip_atoi(&fmt);
			else if (*fmt == '*') {
				fmt++;
				precision = va_arg(args, int);
			}
			if (precision < 0)
				precision = 0;
		}

		qualifier = -1;
		if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' || *fmt == 'Z') {
			qualifier = *fmt;
			fmt++;
		}

		switch (*fmt) {
		case 'c':

			if (!(flags & LEFT))
				while (--field_width > 0)
					*str++ = ' ';
			*str++ = (unsigned char)va_arg(args, int);
			while (--field_width > 0)
				*str++ = ' ';
			break;

		case 's':

			s = va_arg(args, char*);
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
				str = number(str,
					     va_arg(args, unsigned long),
					     8,
					     field_width,
					     precision,
					     flags);
			else
				str = number(str,
					     va_arg(args, unsigned int),
					     8,
					     field_width,
					     precision,
					     flags);
			break;

		case 'p':

			if (field_width == -1) {
				field_width = 2 * sizeof(void*);
				flags |= ZEROPAD;
			}

			str = number(str,
				     (unsigned long)va_arg(args, void*),
				     16,
				     field_width,
				     precision,
				     flags);
			break;

		case 'x':

			flags |= SMALL;

		case 'X':

			if (qualifier == 'l')
				str = number(str,
					     va_arg(args, unsigned long),
					     16,
					     field_width,
					     precision,
					     flags);
			else
				str = number(str,
					     va_arg(args, unsigned int),
					     16,
					     field_width,
					     precision,
					     flags);
			break;

		case 'd':
		case 'i':

			flags |= SIGN;
		case 'u':

			if (qualifier == 'l')
				str = number(str,
					     va_arg(args, unsigned long),
					     10,
					     field_width,
					     precision,
					     flags);
			else
				str = number(str,
					     va_arg(args, unsigned int),
					     10,
					     field_width,
					     precision,
					     flags);
			break;

		case 'n':

			if (qualifier == 'l') {
				long* ip = va_arg(args, long*);
				*ip = (str - buf);
			} else {
				int* ip = va_arg(args, int*);
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

static void printk_emit(const char* s, int len)
{
	int i;

	/* 串口（SBI 控制台）：纯文本 */
	for (i = 0; i < len; i++)
		sbi_putchar(s[i]);

	/* 屏幕gpu_tty：文本只写一次；msg_fg 生效时由 printk 自己
	 * 拼装 \033[<n>m ... \033[0m 包裹，未就绪则整体退化仅串口 */
	if (!printk_screen_ready())
		return;

	if (msg_fg >= 0) {
		char seq[16];
		int n = 0, v = msg_fg;
		char tmp[4];

		/* 进色 \033[<n>m */
		seq[n++] = '\033';
		seq[n++] = '[';
		int m = 0;
		if (v == 0) {
			tmp[m++] = '0';
		} else {
			while (v) {
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

void printk(char* fmt, ...)
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
void printk_color(int fg_sgr, char* fmt, ...)
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

int skip_atoi(const char** s)
{
	int i = 0;

	while (is_digit(**s))
		i = i * 10 + *((*s)++) - '0';
	return i;
}
// do_div
static int do_div(unsigned long long* n, int base)
{
	unsigned long long quotient = *n / base;
	int remainder = (int)(*n - quotient * base); // 等价于 %
	*n = quotient;
	return remainder;
}

static char*
number(char* str, long num, int base, int size, int precision, int type)
{
	char c, sign, tmp[50];
	const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int i;

	if (type & SMALL)
		digits = "0123456789abcdefghijklmnopqrstuvwxyz";
	if (type & LEFT)
		type &= ~ZEROPAD;
	if (base < 2 || base > 36)
		return 0;

	c = (type & ZEROPAD) ? '0' : ' ';
	sign = 0;

	if (type & SIGN && num < 0) {
		sign = '-';
		num = -num;
	} else {
		sign = (type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);
	}

	if (sign)
		size--;
	if (type & SPECIAL) {
		if (base == 16)
			size -= 2;
		else if (base == 8)
			size--;
	}

	i = 0;
	if (num == 0) {
		tmp[i++] = '0';
	} else {
		unsigned long long n = (unsigned long long)num;
		while (n != 0) {
			tmp[i++] = digits[do_div(&n, base)];
		}
	}

	if (i > precision)
		precision = i;
	size -= precision;

	if (!(type & (ZEROPAD + LEFT))) {
		while (size-- > 0)
			*str++ = ' ';
	}

	if (sign)
		*str++ = sign;

	if (type & SPECIAL) {
		if (base == 8)
			*str++ = '0';
		else if (base == 16) {
			*str++ = '0';
			*str++ = digits[33];
		}
	}

	if (!(type & LEFT)) {
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
