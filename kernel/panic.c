#include <panic.h>
#include <printk.h>
#include <stdarg.h>
#include <lib.h>
#include <tty/gpu_tty.h>

void print_notice_flag(int panic_id)
{
	printk("[");
	switch (panic_id) {
	case PANIC_ERROR: // error
		printk("ERROR");
		break;
	case PANIC_WRONG: // wrong
		printk("WRONG");
		break;
	default:
		printk("!");
		break;
	}
	printk("] ");
}

void panic(int panic_id, char* reason, ...)
{
	char buf[PRINT_BUFFER_SIZE];
	int off = 0;
	va_list args;

	buf[off++] = '[';
	switch (panic_id) {
	case PANIC_ERROR:
		strcpy(buf + off, "ERROR");
		off += 5;
		break;
	case PANIC_WRONG:
		strcpy(buf + off, "WRONG");
		off += 5;
		break;
	default:
		buf[off++] = '!';
		break;
	}
	buf[off++] = ']';
	buf[off++] = ' ';

	va_start(args, reason);
	vsprintf(buf + off, reason, args);
	va_end(args);

	/* panic 消息红色（SGR 31，对应 PANIC_FRONT_COLOR=RED），
	 * 屏幕醒目；串口侧保持纯文本 */
	printk_color(31, "%s", buf);

	if (panic_id == PANIC_ERROR) {
		/* 停机前强制上屏：此后时钟节拍不再推进，脏区永远刷不出去 */
		gpu_tty_console_flush();
		while (1)
			;
	}
}

void panic_wrong(char* reason, ...)
{
	va_list args;
	char buf[PRINT_BUFFER_SIZE];

	va_start(args, reason);
	vsprintf(buf, reason, args);
	va_end(args);

	panic(PANIC_WRONG, "%s", buf);
}

void panic_error(char* reason, ...)
{
	va_list args;
	char buf[PRINT_BUFFER_SIZE];

	va_start(args, reason);
	vsprintf(buf, reason, args);
	va_end(args);

	panic(PANIC_ERROR, "%s", buf);
}