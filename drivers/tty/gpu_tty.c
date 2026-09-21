#include <tty.h>
#include <virtio_gpu.h>
#include <lib.h>
#include <printk.h>
#include <spinlock.h>
#include <tty/gpu_tty.h>
#include <kgfx.h>
#include <ascii8x16.h>

static void gpu_tty_advance_line(struct gpu_tty_state *st);
static void gpu_tty_test(void);


static void gpu_tty_advance_line(struct gpu_tty_state *st)
{
        st->cur_x = 0;
        st->cur_y += ASCII8X16_H;

        if (st->cur_y + ASCII8X16_H > st->gpu->height) {
                kgfx_clear(st->gpu, st->bg);
                st->cur_x = 0;
                st->cur_y = 0;
        }
}

static int gpu_tty_putc(struct tty *tty, char c)
{
        struct gpu_tty_state *st = tty->priv;
        struct virtio_gpu_device *gpu = st->gpu;

        if (c == '\r') {
                st->cur_x = 0;
                return 0;
        }

        if (c == '\n') {
                gpu_tty_advance_line(st);
                kgfx_update(gpu);
                return 0;
        }

        if (c == '\b') {
                if (st->cur_x >= ASCII8X16_W)
                st->cur_x -= ASCII8X16_W;
                return 0;
        }

        if (st->cur_x + ASCII8X16_W > gpu->width)
                gpu_tty_advance_line(st);

        kgfx_draw_char(gpu, c, st->cur_x, st->cur_y, st->fg, st->bg);
        st->cur_x += ASCII8X16_W;

        kgfx_update(gpu);
        return 0;
}

static int gpu_tty_getc(struct tty *tty, char *out)
{
        return 0;
}

static int gpu_tty_has_input(struct tty *tty)
{
        return 0;
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
static struct gpu_tty_state gpu0_tty_state;

void init_gpu_tty()
{
        
        gpu0_tty_state.gpu   = gpu_get(VIRTIO_GPU_SELECT_IDX);
        gpu0_tty_state.cur_x = 0;
        gpu0_tty_state.cur_y = 0;
        gpu0_tty_state.fg    = 0xFFFFFFFF;
        gpu0_tty_state.bg    = 0xFF000000; 

        kgfx_clear(gpu0_tty_state.gpu, gpu0_tty_state.bg);
        kgfx_update(gpu0_tty_state.gpu);

        gpu0_tty.name = "gpu/0";
        gpu0_tty.ops  = &gpu_tty_ops;
        gpu0_tty.priv = &gpu0_tty_state;
        gpu0_tty.linepos    = 0;
        gpu0_tty.line_ready = 0;
        init_spinlock(&gpu0_tty.read_lock);
        init_spinlock(&gpu0_tty.output_lock);
        gpu0_tty.read_chan = &gpu0_tty.read_chan;

        if (tty_register(&gpu0_tty) < 0) {
                printk("init_gpu_tty: register failed\n");
                return;
        }
        printk("the gpu tty has been initied!\n");
        gpu_tty_test();
}

static void gpu_tty_test(void)
{
        struct tty *t = &gpu0_tty;

        t->ops->putc(t, 'H');
        t->ops->putc(t, 'e');
        t->ops->putc(t, 'l');
        t->ops->putc(t, 'l');
        t->ops->putc(t, 'o');
        t->ops->putc(t, ',');
        t->ops->putc(t, ' ');
        t->ops->putc(t, 'G');
        t->ops->putc(t, 'P');
        t->ops->putc(t, 'U');
        t->ops->putc(t, '!');
        t->ops->putc(t, '\n');

        const char *lines[] = {
                "Line 01: ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                "Line 02: abcdefghijklmnopqrstuvwxyz",
                "Line 03: 0123456789",
                "Line 04: !@#$%^&*()_+-=[]{}|;':\",./<>?",
                "Line 05: The quick brown fox jumps over the lazy dog",
        };

        for (int i = 0; i < 5; i++) {
                const char *s = lines[i];
                while (*s) {
                        t->ops->putc(t, *s);
                        s++;
                }
                t->ops->putc(t, '\n');
        }

        // 测试 \b：打错了退格
        t->ops->putc(t, 'A');
        t->ops->putc(t, 'B');
        t->ops->putc(t, 'C');
        t->ops->putc(t, '\b');
        t->ops->putc(t, '\b');
        t->ops->putc(t, 'D');
        t->ops->putc(t, 'E');
        t->ops->putc(t, '\n');
}