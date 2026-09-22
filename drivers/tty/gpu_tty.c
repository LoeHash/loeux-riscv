#include <tty.h>
#include <keyboard.h>
#include <virtio_gpu.h>
#include <uart.h>
#include <lib.h>
#include <printk.h>
#include <spinlock.h>
#include <tty/gpu_tty.h>
#include <kgfx.h>
#include <ascii8x16.h>
/**
        gpu_tty 终端设备驱动
                实现了 gpu tty 的基本功能，包括字符绘制、光标移动、换行等。
                注意: 这玩意是一套驱动的集合体，如终端输出是用 kgfx->virtio_gpu 驱动
                     而读取输入，会通过键盘驱动来实现
                     同时这个终端设备也需要视觉反馈，并非像以前的uart tty 那样直接输出到宿主机终端
                     这也为未来脱离宿主机，真正的运行在物理机上提供了可能！
**/

static void gpu_tty_advance_line(struct gpu_tty_state *st);
static void gpu_tty_test(void);
static void gpu_tty_erase_cursor(struct gpu_tty_state *st);
static void gpu_tty_draw_cursor(struct gpu_tty_state *st);

/*
 * 光标擦/画只写后端 fb 并标脏，不主动刷屏。
 * 真正的 GPU 传输统一由 gpu_tty_flush 完成：
 * 一次批量输出只产生一次 TRANSFER_TO_HOST_2D + RESOURCE_FLUSH，
 * 避免 QEMU 端每个字符都做一次窗口重绘（这是之前卡顿的根源）。
 */
static void gpu_tty_draw_cursor(struct gpu_tty_state *st)
{
	if (st->cursor_drawn)
		return;

	kgfx_fill_rect(st->gpu, st->cur_x, st->cur_y + 14,
	               ASCII8X16_W, 2, 0xFFFFFFFF);
	st->cursor_drawn = 1;
}

static void gpu_tty_erase_cursor(struct gpu_tty_state *st)
{
	if (!st->cursor_drawn)
		return;

	kgfx_fill_rect(st->gpu, st->cur_x, st->cur_y + 14,
	               ASCII8X16_W, 2, st->bg);
	st->cursor_drawn = 0;
}

static void gpu_tty_advance_line(struct gpu_tty_state *st)
{
        struct virtio_gpu_device *gpu = st->gpu;

        st->cur_x = 0;
        st->cur_y += ASCII8X16_H;

        if (st->cur_y + ASCII8X16_H > gpu->height) {
                /*
                 * 到底：向上滚动一行，保留历史内容（原来的做法是
                 * kgfx_clear 清全屏从头开始，终端历史全部丢失）。
                 * putc 总是先擦光标再走这里，所以不会有白色
                 * 光标线被一起滚上去。
                 */
                kgfx_scroll_up(gpu, ASCII8X16_H, st->bg);
                st->cur_y = gpu->height - ASCII8X16_H;
        }
}

static int gpu_tty_putc(struct tty *tty, char c)
{
	struct gpu_tty_state *st = tty->priv;
	struct virtio_gpu_device *gpu = st->gpu;

	gpu_tty_erase_cursor(st);

	if (c == '\r') {
		st->cur_x = 0;
	} else if (c == '\n') {
		gpu_tty_advance_line(st);
	} else if (c == '\b') {
		if (st->cur_x >= ASCII8X16_W)
			st->cur_x -= ASCII8X16_W;
	} else {
		if (st->cur_x + ASCII8X16_W > gpu->width)
			gpu_tty_advance_line(st);

		kgfx_draw_char(gpu, c, st->cur_x, st->cur_y, st->fg, st->bg);
		st->cur_x += ASCII8X16_W;
	}

	if (st->cursor_visible)
		gpu_tty_draw_cursor(st);

	return 0;
}

/* 把标脏的后端缓冲一次性刷到屏幕 */
static int gpu_tty_flush(struct tty *tty)
{
	struct gpu_tty_state *st = tty->priv;
	kgfx_update(st->gpu);
	return 0;
}

static int gpu_tty_getc(struct tty *tty, char *out)
{
	return keyboard_getchar(out);
}

static int gpu_tty_has_input(struct tty *tty)
{
        (void)tty;
        return keyboard_has_input();
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
        .flush     = gpu_tty_flush,
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
        gpu0_tty_state.cursor_visible = 1;
        gpu0_tty_state.cursor_drawn = 0;
        gpu0_tty_state.blink_counter = 0;

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

        /* 批量输出结束，一次性刷屏 */
        t->ops->flush(t);
}