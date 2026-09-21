#include <tty.h>
#include <virtio_gpu.h>
#include <lib.h>
#include <printk.h>
#include <spinlock.h>
#include <tty/gpu_tty.h>


static int gpu_tty_putc(struct tty *tty, char c)
{
//     uart_putchar(c);
    return 0;
}

static int gpu_tty_getc(struct tty *tty, char *out)
{
//     *out = uart_getchar();
    return 0;
}

static int gpu_tty_has_input(struct tty *tty)
{
//     return uart_available();
}

static int gpu_tty_open(struct tty *tty, int flags)
{
    return 0;
}

static int gpu_tty_close(struct tty *tty)
{
    return 0;
}

static const struct tty_ops gpu_tty_ops = {
    .open      = gpu_tty_open,
    .close     = gpu_tty_close,
    .putc      = gpu_tty_putc,
    .getc      = gpu_tty_getc,
    .has_input = gpu_tty_has_input,
};

static struct tty gpu0_tty;

void init_gpu_tty(void)
{
    gpu0_tty.name = "gpu/0";
    gpu0_tty.ops  = &gpu_tty_ops;
    gpu0_tty.priv = NULL;

    gpu0_tty.linepos    = 0;
    gpu0_tty.line_ready = 0;

    init_spinlock(&gpu0_tty.read_lock);
    init_spinlock(&gpu0_tty.output_lock);

    gpu0_tty.read_chan = &gpu0_tty.read_chan;

    if (tty_register(&gpu0_tty) < 0) {
        printk("init_gpu_tty: register failed\n");
        return;
    }
}