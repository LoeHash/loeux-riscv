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
		注意: 这玩意是一套驱动的集合体，如终端输出是用 kgfx->virtio_gpu
驱动 驱动 而读取输入，会通过键盘驱动来实现
		     同时这个终端设备也需要视觉反馈，并非像以前的uart tty
那样直接输出到宿主机终端 这也为未来脱离宿主机，真正的运行在物理机上提供了可能！
**/

static void gpu_tty_advance_line(struct gpu_tty_state* st);
static void gpu_tty_test(void);
static void gpu_tty_erase_cursor(struct gpu_tty_state* st);
static void gpu_tty_draw_cursor(struct gpu_tty_state* st);
static void parse_two(const char* s, int* a, int* b, int da, int db);
static void
gpu_tty_handle_csi(struct gpu_tty_state* st, char final, const char* args);

static void parse_two(const char* s, int* a, int* b, int da, int db)
{
	int v = 0, first = 1, has = 0;
	*a = da;
	*b = db;
	for (; *s; s++) {
		if (*s >= '0' && *s <= '9') {
			v = v * 10 + (*s - '0');
			has = 1;
		} else if (*s == ';') {
			if (first) {
				*a = has ? v : da;
				first = 0;
			}
			v = 0;
			has = 0;
		}
	}
	if (first) {
		if (has)
			*a = v;
	} else {
		if (has)
			*b = v;
	}
}

static void
gpu_tty_handle_csi(struct gpu_tty_state* st, char final, const char* args)
{
	struct virtio_gpu_device* gpu = st->gpu;

	switch (final) {
	case 'J': {
		int mode = 0;
		if (args[0] >= '0' && args[0] <= '9')
			mode = args[0] - '0';
		if (mode == 2) {
			/* 全屏清成背景色，光标归零 */
			kgfx_fill_rect_nodirty(
			    gpu, 0, 0, gpu->width, gpu->height, st->bg);
			st->cur_x = 0;
			st->cur_y = 0;
			st->cursor_drawn = 0;
			kgfx_mark_dirty_screen(0, 0, gpu->width, gpu->height);
			if (st->cursor_visible)
				gpu_tty_draw_cursor(st);
		}
		break;
	}
	case 'H':
	case 'f': {
		int row = 1, col = 1;
		parse_two(args, &row, &col, 1, 1);
		if (row < 1)
			row = 1;
		if (col < 1)
			col = 1;
		uint32_t nx = (uint32_t)(col - 1) * ASCII8X16_W;
		uint32_t ny = (uint32_t)(row - 1) * ASCII8X16_H;
		if (nx + ASCII8X16_W > gpu->width)
			nx = gpu->width - ASCII8X16_W;
		if (ny + ASCII8X16_H > gpu->height)
			ny = gpu->height - ASCII8X16_H;
		gpu_tty_erase_cursor(st);
		st->cur_x = nx;
		st->cur_y = ny;
		if (st->cursor_visible)
			gpu_tty_draw_cursor(st);
		kgfx_mark_dirty_screen(nx, ny, ASCII8X16_W, ASCII8X16_H);
		break;
	}
	case 'K': {
		int mode = 0;
		if (args[0] >= '0' && args[0] <= '9')
			mode = args[0] - '0';
		uint32_t x0 = st->cur_x;
		uint32_t x1 = gpu->width;
		if (mode == 1) {
			x0 = 0;
			x1 = st->cur_x + ASCII8X16_W;
		} else if (mode == 2) {
			x0 = 0;
			x1 = gpu->width;
		}
		if (x1 > x0) {
			gpu_tty_erase_cursor(st);
			kgfx_fill_rect_nodirty(
			    gpu, x0, st->cur_y, x1 - x0, ASCII8X16_H, st->bg);
			kgfx_mark_dirty_screen(
			    x0, st->cur_y, x1 - x0, ASCII8X16_H);
			if (st->cursor_visible)
				gpu_tty_draw_cursor(st);
		}
		break;
	}
	case 'l':
	case 'h': {
		/* \033[?25l / \033[?25h */
		if (args[0] == '?' && args[1] == '2' && args[2] == '5') {
			if (final == 'l') {
				gpu_tty_erase_cursor(st);
				st->cursor_visible = 0;
			} else {
				st->cursor_visible = 1;
				gpu_tty_draw_cursor(st);
			}
			kgfx_mark_dirty_screen(
			    st->cur_x, st->cur_y + 14, ASCII8X16_W, 2);
		}
		break;
	}
	default:
		break;
	}
}

/*
 * 光标擦/画用 nodirty 路径：putc 末尾一次性 mark_dirty，
 * 3 次锁→1 次，热路径锁开销降 2/3。
 */
static void gpu_tty_draw_cursor(struct gpu_tty_state* st)
{
	if (st->cursor_drawn)
		return;

	kgfx_fill_rect_nodirty(
	    st->gpu, st->cur_x, st->cur_y + 14, ASCII8X16_W, 2, 0xFFFFFFFF);
	st->cursor_drawn = 1;
}

static void gpu_tty_erase_cursor(struct gpu_tty_state* st)
{
	if (!st->cursor_drawn)
		return;

	kgfx_fill_rect_nodirty(
	    st->gpu, st->cur_x, st->cur_y + 14, ASCII8X16_W, 2, st->bg);
	st->cursor_drawn = 0;
}

static void gpu_tty_advance_line(struct gpu_tty_state* st)
{
	struct virtio_gpu_device* gpu = st->gpu;

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

static int gpu_tty_putc(struct tty* tty, char c)
{
	struct gpu_tty_state* st = tty->priv;
	struct virtio_gpu_device* gpu = st->gpu;

	if (st->esc_state) {
		if (st->esc_state == 1) {
			if (c == '[') {
				st->esc_state = 2;
				st->esc_len = 0;
				return 0;
			}
			st->esc_state = 0;
		} else {
			if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
				if (st->esc_len < 15)
					st->esc_buf[st->esc_len++] = c;
				return 0;
			}
			st->esc_buf[st->esc_len] = '\0';
			gpu_tty_handle_csi(st, c, st->esc_buf);
			st->esc_state = 0;
			return 0;
		}
	}
	if (c == '\033') {
		st->esc_state = 1;
		return 0;
	}
	/* ---- 原有逻辑 ---- */

	gpu_tty_erase_cursor(st);
	uint32_t draw_x = st->cur_x;
	uint32_t draw_y = st->cur_y;

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
		kgfx_draw_char_nodirty(
		    gpu, c, st->cur_x, st->cur_y, st->fg, st->bg);
		st->cur_x += ASCII8X16_W;
	}

	if (st->cursor_visible) {
		kgfx_fill_rect_nodirty(st->gpu,
				       st->cur_x,
				       st->cur_y + 14,
				       ASCII8X16_W,
				       2,
				       0xFFFFFFFF);
		st->cursor_drawn = 1;
	}

	uint32_t x0 = draw_x;
	uint32_t x1 = st->cur_x + ASCII8X16_W;
	uint32_t y0 = draw_y;
	uint32_t y1 = st->cur_y + ASCII8X16_H;
	kgfx_mark_dirty_screen(x0, y0, x1 - x0, y1 - y0);

	return 0;
}

/* 立即把标脏的后端缓冲刷到屏幕*/
static int gpu_tty_flush(struct tty* tty)
{
	(void)tty;
	kgfx_flush_now();
	return 0;
}

static int gpu_tty_getc(struct tty* tty, char* out)
{
	return keyboard_getchar(out);
}

static int gpu_tty_has_input(struct tty* tty)
{
	(void)tty;
	return keyboard_has_input();
}

static int gpu_tty_open(struct tty* tty, int flags)
{
	return 0;
}

static int gpu_tty_close(struct tty* tty)
{
	return 0;
}

static const struct tty_ops gpu_tty_ops = {
    .open = gpu_tty_open,
    .close = gpu_tty_close,
    .putc = gpu_tty_putc,
    .getc = gpu_tty_getc,
    .has_input = gpu_tty_has_input,
    .flush = gpu_tty_flush,
};

static struct tty gpu0_tty;
static struct gpu_tty_state gpu0_tty_state;

void gpu_tty_tick()
{
	struct tty* tty = &gpu0_tty;
	struct gpu_tty_state* st = tty->priv;

	st->blink_counter++;
	if (st->blink_counter < BLINK_INTERVAL)
		return;

	st->blink_counter = 0;
	st->cursor_visible = !st->cursor_visible;

	/* nodirty 擦/画 + 一次 mark_dirty：只标 8x2 条带 */
	if (st->cursor_visible)
		gpu_tty_draw_cursor(st);
	else
		gpu_tty_erase_cursor(st);

	kgfx_mark_dirty_screen(st->cur_x, st->cur_y + 14, ASCII8X16_W, 2);
}

void init_gpu_tty()
{

	gpu0_tty_state.gpu = gpu_get(VIRTIO_GPU_SELECT_IDX);
	gpu0_tty_state.cur_x = 0;
	gpu0_tty_state.cur_y = 0;
	gpu0_tty_state.fg = 0xFFFFFFFF;
	gpu0_tty_state.bg = 0xFF000000;
	gpu0_tty_state.cursor_visible = 1;
	gpu0_tty_state.cursor_drawn = 0;
	gpu0_tty_state.blink_counter = 0;
	gpu0_tty_state.esc_state = 0;
	gpu0_tty_state.esc_len = 0;

	/* 注册给 kgfx，之后屏幕刷新由 100Hz 时钟节拍统一合并驱动 */
	kgfx_attach(gpu0_tty_state.gpu);

	kgfx_clear(gpu0_tty_state.gpu, gpu0_tty_state.bg);
	kgfx_flush_now();

	gpu0_tty.name = "gpu/0";
	gpu0_tty.ops = &gpu_tty_ops;
	gpu0_tty.priv = &gpu0_tty_state;
	gpu0_tty.linepos = 0;
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
	struct tty* t = &gpu0_tty;

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

	const char* lines[] = {
	    "Line 01: ABCDEFGHIJKLMNOPQRSTUVWXYZ",
	    "Line 02: abcdefghijklmnopqrstuvwxyz",
	    "Line 03: 0123456789",
	    "Line 04: !@#$%^&*()_+-=[]{}|;':\",./<>?",
	    "Line 05: The quick brown fox jumps over the lazy dog",
	};

	for (int i = 0; i < 5; i++) {
		const char* s = lines[i];
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
