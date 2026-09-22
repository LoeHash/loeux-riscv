#include <tty.h>
#include <uart.h>
#include <lib.h>
#include <printk.h>
#include <spinlock.h>
#include <tty/uart_tty.h>

static int uart_tty_putc(struct tty* tty, char c)
{
	uart_putchar(c);
	return 0;
}

static int uart_tty_getc(struct tty* tty, char* out)
{
	*out = uart_getchar();
	return 0;
}

static int uart_tty_has_input(struct tty* tty)
{
	return uart_available();
}

static int uart_tty_open(struct tty* tty, int flags)
{
	return 0;
}

static int uart_tty_close(struct tty* tty)
{
	return 0;
}

static const struct tty_ops uart_tty_ops = {
    .open = uart_tty_open,
    .close = uart_tty_close,
    .putc = uart_tty_putc,
    .getc = uart_tty_getc,
    .has_input = uart_tty_has_input,
};

static struct tty uart0_tty;

void init_uart_tty(void)
{
	uart0_tty.name = "uart/0";
	uart0_tty.ops = &uart_tty_ops;
	uart0_tty.priv = NULL;

	uart0_tty.linepos = 0;
	uart0_tty.line_ready = 0;

	init_spinlock(&uart0_tty.read_lock);
	init_spinlock(&uart0_tty.output_lock);

	uart0_tty.read_chan = &uart0_tty.read_chan;

	if (tty_register(&uart0_tty) < 0) {
		printk("init_uart_tty: register failed\n");
		return;
	}
}