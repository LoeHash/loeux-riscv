#include <drivers/gpu/kgfx.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>
#include <drivers/gpu/virtio_gpu.h>
#include <utils/math.h>
#include <lib.h>
#include <ascii8x16.h>

/*
 * 坐标系约定：
 *   - 所有绘图接口（put_pixel/draw_char/fill_rect/...）接收的都是
 *     屏幕坐标，范围 (0,0) .. width,height
 *   - backing framebuffer / virtio resource 的实际高度是 2*height，
 *     屏幕只是其中通过 SET_SCANOUT 开出的一个高度为 height 的窗口，
 *     窗口顶端的行偏移叫 pan_y（panning）。
 *   - 写 fb 与标脏一律用 resource 绝对坐标：py = screen_y + pan_y。
 *
 * 滚屏
 *   滚一行时不搬像素，只把 pan_y 下移 line_h 并用 SET_SCANOUT 平移窗口，
 *   旧行本来就在 resource 里，只需传输新露出的底部条带（~20KB，
 *   而非整屏 4MB）。窗口移到 backing 底部后，做一次 memmove 回绕 +
 *   全屏传输
 */

// 脏区域（resource 绝对坐标）
static spinlock_t dirty_lock = {0};
static uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static int dirty_valid;
/* 本帧是否需要 SET_SCANOUT 平移窗口，以及目标 pan_y */
static int pan_changed;
static uint32_t pan_y;

/* 当前屏幕（单屏），时钟节拍刷新用；attach 前为 NULL */
static struct virtio_gpu_device* kgfx_gpu;
/* 上一次刷屏时的系统拍数，用于定时路径节流 */
static uint64_t last_flush_tick;
/* 定时刷新间隔（拍）。100Hz 下 1 拍 = 100fps，最大化响应速度 */
#define KGFX_FLUSH_INTERVAL_TICKS 1

static void mark_dirty_locked(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* 屏幕坐标 -> resource 绝对行。pan_y 仅在 dirty_lock 内被改写，
 * 对齐 32 位读写本身原子，绘制路径直接读即可。 */
static inline uint32_t kgfx_pan(void)
{
	return pan_y;
}

void init_kgfx(void)
{
	init_spinlock(&dirty_lock);
	dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
	dirty_valid = 0;
	pan_changed = 0;
	pan_y = 0;
	kgfx_gpu = NULL;
}

void kgfx_attach(struct virtio_gpu_device* gpu)
{
	kgfx_gpu = gpu;
}

void kgfx_put_pixel(struct virtio_gpu_device* gpu,
		    uint32_t x,
		    uint32_t y,
		    uint32_t color)
{
	if (gpu == NULL || gpu->fb == NULL)
		return;
	if (x >= gpu->width || y >= gpu->height)
		return;

	uint32_t py = y + kgfx_pan();
	((uint32_t*)gpu->fb)[(uint64_t)py * gpu->width + x] = color;

	mark_dirty(x, py, 1, 1);
}

void kgfx_clear(struct virtio_gpu_device* gpu, uint32_t color)
{
	if (gpu == NULL || gpu->fb == NULL)
		return;

	/* 清整块 backing（含 panning 余量），宽字 memset */
	memset(gpu->fb,
	       (int)(uint8_t)color,
	       (uint64_t)gpu->width * gpu->fb_height * sizeof(uint32_t));

	acquire(&dirty_lock);
	pan_y = 0;
	pan_changed = 1;
	mark_dirty_locked(0, 0, gpu->width, gpu->height);
	release(&dirty_lock);
}

void kgfx_scroll_up(struct virtio_gpu_device* gpu, uint32_t line_h, uint32_t bg)
{
	if (gpu == NULL || gpu->fb == NULL)
		return;
	if (line_h == 0 || line_h > gpu->height)
		return;

	uint32_t W = gpu->width;
	uint32_t H = gpu->height;
	uint32_t FBH = gpu->fb_height;
	uint32_t* fb = (uint32_t*)gpu->fb;
	uint64_t screen_px = (uint64_t)W * H;
	uint64_t line_px = (uint64_t)W * line_h;

	/*
	 * 全部内存改动 + pan 更新 + 标脏都在 dirty_lock 内完成，
	 * 避免定时器中断在"已标脏但像素还没写完"的窗口里把半帧刷走。
	 * 持锁会关本核中断：常规帧只填 20KB 条带；回绕帧一次 4MB
	 * memmove（每 H/line_h 行才一次），可接受。
	 */
	acquire(&dirty_lock);

	if (pan_y + H + line_h > FBH) {
		/*
		 * 窗口已到底（pan_y==H），无法再下移：
		 * 把当前窗口 [H,2H) 的内容整块搬到 [0,H)，窗口归零，
		 * 然后整屏重传一次。每滚 H/line_h 行才发生一次。
		 */
		memmove(fb, fb + screen_px, screen_px * sizeof(uint32_t));
		pan_y = 0;
	} else {
		/*
		 * 常规 panning：窗口下移一行，旧像素零搬运。
		 */
		pan_y += line_h;
	}

	/*
	 * 新底部行填背景色（常规帧该行 backing 未用过；回绕帧它是
	 * 旧窗口内容，都必须清）。起点 8 字节对齐，按 64 位双像素写。
	 */
	{
		uint32_t band_y = pan_y + H - line_h;
		uint64_t bgpair = (uint64_t)bg | ((uint64_t)bg << 32);
		uint64_t* bot = (uint64_t*)(fb + (uint64_t)band_y * W);
		for (uint64_t i = 0; i < line_px / 2; i++)
			bot[i] = bgpair;
	}

	/*
	 * 回绕帧：memmove 后旧坐标内容已失效，且旧脏区（擦光标等）
	 * 落在 backing 高位、不在新窗口内——脏区直接重置为单屏，
	 * 不能 union，否则会 union 出覆盖整块 2H backing 的脏矩形，
	 * 平白多传 4MB。常规帧只脏新露出的底部条带。
	 */
	if (pan_y == 0) {
		dirty_valid = 1;
		dirty_x0 = 0;
		dirty_y0 = 0;
		dirty_x1 = W;
		dirty_y1 = H;
	} else {
		mark_dirty_locked(0, pan_y + H - line_h, W, line_h);
	}
	pan_changed = 1;
	release(&dirty_lock);
}

/* ---- 不标脏的绘图（高速批量路径） ---- */

/*
 * 宽字 fill_rect：按行用 uint64 双像素写，store 数减半。
 * 当宽度为偶数时全走 uint64；奇数尾部补一个 uint32。
 * 调用者负责 mark_dirty。
 */
void kgfx_fill_rect_nodirty(struct virtio_gpu_device* gpu,
			    uint32_t x,
			    uint32_t y,
			    uint32_t w,
			    uint32_t h,
			    uint32_t color)
{
	if (gpu == NULL || gpu->fb == NULL)
		return;
	if (w == 0 || h == 0)
		return;

	uint32_t x1 = x + w;
	uint32_t y1 = y + h;
	if (x1 > gpu->width)
		x1 = gpu->width;
	if (y1 > gpu->height)
		y1 = gpu->height;
	if (x >= x1 || y >= y1)
		return;

	uint32_t pan = kgfx_pan();
	uint32_t rw = x1 - x;
	uint32_t pairs = rw / 2;
	uint32_t tail = rw & 1;
	uint64_t pair = (uint64_t)color | ((uint64_t)color << 32);

	for (uint32_t row = y; row < y1; row++) {
		uint32_t* line = (uint32_t*)gpu->fb +
				 (uint64_t)(row + pan) * gpu->width + x;
		uint64_t* p64 = (uint64_t*)line;
		for (uint32_t i = 0; i < pairs; i++)
			p64[i] = pair;
		if (tail)
			line[pairs * 2] = color;
	}
}

/*
 * 宽字 draw_char：每行 8 像素用 4 次 uint64 store 代替 8 次 uint32 store。
 * x 是 ASCII8X16_W(=8) 的倍数，所以 8 像素 = 32 字节，uint64 对齐。
 * 调用者负责 mark_dirty。
 */
static void draw_char_raw(struct virtio_gpu_device* gpu,
			  char c,
			  uint32_t x,
			  uint32_t y,
			  uint32_t fg,
			  uint32_t bg)
{
	const uint8_t* glyph = ascii8x16[(uint8_t)c];
	uint32_t* fb = (uint32_t*)gpu->fb;
	uint32_t W = gpu->width;
	uint32_t H = gpu->height;
	uint32_t pan = kgfx_pan();

	for (int row = 0; row < ASCII8X16_H; row++) {
		uint32_t sy = y + row;
		if (sy >= H)
			break;

		uint8_t bits = glyph[row];
		uint64_t* line64 =
		    (uint64_t*)(fb + (uint64_t)(sy + pan) * W + x);

		/* 8 像素 = 4 个 uint64（每 2 像素一次） */
		for (int i = 0; i < 4; i++) {
			uint8_t b = bits >> (6 - i * 2);
			uint64_t p = (b & 0x80) ? fg : bg;
			p |= (uint64_t)((b & 0x40) ? fg : bg) << 32;
			line64[i] = p;
		}
	}
}

void kgfx_draw_char_nodirty(struct virtio_gpu_device* gpu,
			   char c,
			   uint32_t x,
			   uint32_t y,
			   uint32_t fg,
			   uint32_t bg)
{
	if (gpu == NULL || gpu->fb == NULL)
		return;
	if (x >= gpu->width || y >= gpu->height)
		return;
	draw_char_raw(gpu, c, x, y, fg, bg);
}

/* ---- 标脏接口 ---- */

void kgfx_mark_dirty_screen(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	uint32_t pan = kgfx_pan();
	mark_dirty(x, y + pan, w, h);
}

/* ---- 标脏的绘图（对外接口，含 mark_dirty） ---- */

void kgfx_fill_rect(struct virtio_gpu_device* gpu,
		    uint32_t x,
		    uint32_t y,
		    uint32_t w,
		    uint32_t h,
		    uint32_t color)
{
	kgfx_fill_rect_nodirty(gpu, x, y, w, h, color);
	uint32_t pan = kgfx_pan();
	mark_dirty(x, y + pan, w, h);
}

void kgfx_draw_rect(struct virtio_gpu_device* gpu,
		    uint32_t x,
		    uint32_t y,
		    uint32_t w,
		    uint32_t h,
		    uint32_t color)
{
	if (w == 0 || h == 0)
		return;

	// 上边
	kgfx_fill_rect(gpu, x, y, w, 1, color);
	// 下边
	kgfx_fill_rect(gpu, x, y + h - 1, w, 1, color);
	// 左边
	kgfx_fill_rect(gpu, x, y, 1, h, color);
	// 右边
	kgfx_fill_rect(gpu, x + w - 1, y, 1, h, color);
}

void kgfx_draw_char(struct virtio_gpu_device* gpu,
		    char c,
		    uint32_t x,
		    uint32_t y,
		    uint32_t fg,
		    uint32_t bg)
{
	if (gpu == NULL || gpu->fb == NULL)
		return;
	if (x >= gpu->width || y >= gpu->height)
		return;

	draw_char_raw(gpu, c, x, y, fg, bg);
	kgfx_mark_dirty_screen(x, y, ASCII8X16_W, ASCII8X16_H);
}

void kgfx_draw_string(struct virtio_gpu_device* gpu,
		      const char* s,
		      uint32_t x,
		      uint32_t y,
		      uint32_t fg,
		      uint32_t bg)
{
	if (gpu == NULL || gpu->fb == NULL || s == NULL)
		return;
	if (x >= gpu->width || y >= gpu->height)
		return;

	uint32_t start_x = x;

	while (*s) {
		if (x + ASCII8X16_W > gpu->width)
			break; // 超出右边界，停止

		draw_char_raw(gpu, *s, x, y, fg, bg);
		x += ASCII8X16_W;
		s++;
	}

	if (x > start_x)
		kgfx_mark_dirty_screen(start_x, y, x - start_x,
				       ASCII8X16_H);
}

/*
 * 绘制路径只标脏（mark_dirty 已在各绘图函数里完成累积），
 * 这里不立即发 virtio 命令。刷屏统一由 100Hz 时钟节拍合并节流完成，
 * 或由 kgfx_flush_now 在需要立即上屏时（如打字回显）强制完成。
 */
void kgfx_update(struct virtio_gpu_device* gpu)
{
	(void)gpu;
}

/* 锁内快照脏区/pan 状态并清零；无脏区返回 0 */
static int kgfx_take_frame(uint32_t* x0,
			   uint32_t* y0,
			   uint32_t* x1,
			   uint32_t* y1,
			   int* rescan,
			   uint32_t* scanout_y)
{
	int have;

	acquire(&dirty_lock);
	have = dirty_valid;
	if (have) {
		*x0 = dirty_x0;
		*y0 = dirty_y0;
		*x1 = dirty_x1;
		*y1 = dirty_y1;
		dirty_valid = 0;
		*rescan = pan_changed;
		*scanout_y = pan_y;
		pan_changed = 0;
	}
	release(&dirty_lock);
	return have;
}

void kgfx_flush_now(void)
{
	struct virtio_gpu_device* gpu = kgfx_gpu;
	uint32_t x0, y0, x1, y1, sy;
	int rescan;

	if (gpu == NULL)
		return;
	if (!kgfx_take_frame(&x0, &y0, &x1, &y1, &rescan, &sy))
		return;

	/* 锁外发命令：慢操作，不占 dirty_lock */
	virtio_gpu_present(gpu, x0, y0, x1 - x0, y1 - y0, rescan, sy);
	last_flush_tick = get_sys_timer_tick();
}

/*
 * 时钟节拍回调（100Hz 驱动，1 tick = 100fps）。
 * 多个 hart 都会进时钟中断；kgfx_take_frame 在锁内把帧状态取走，
 * 因此同一帧只有一个 hart 真正发命令。
 */
void kgfx_timer_tick(void)
{
	if (kgfx_gpu == NULL || !dirty_valid)
		return;
	if (get_sys_timer_tick() - last_flush_tick < KGFX_FLUSH_INTERVAL_TICKS)
		return;
	kgfx_flush_now();
}

static void mark_dirty_locked(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0)
		return;

	uint32_t x1 = x + w;
	uint32_t y1 = y + h;

	if (!dirty_valid) {
		dirty_x0 = x;
		dirty_y0 = y;
		dirty_x1 = x1;
		dirty_y1 = y1;
		dirty_valid = 1;
		return;
	}

	dirty_x0 = min(dirty_x0, x);
	dirty_y0 = min(dirty_y0, y);
	dirty_x1 = max(dirty_x1, x1);
	dirty_y1 = max(dirty_y1, y1);
}

// 标记脏区域（resource 绝对坐标）
static void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	acquire(&dirty_lock);
	mark_dirty_locked(x, y, w, h);
	release(&dirty_lock);
}

void kgfx_draw_line(struct virtio_gpu_device* gpu,
		    int32_t x0,
		    int32_t y0,
		    int32_t x1,
		    int32_t y1,
		    uint32_t color)
{
	if (gpu == NULL || gpu->fb == NULL)
		return;

	uint32_t W = gpu->width;
	uint32_t H = gpu->height;
	uint32_t* fb = (uint32_t*)gpu->fb;
	uint32_t pan = kgfx_pan();

	int32_t dx = abs(x1 - x0);
	int32_t dy = -abs(y1 - y0);
	int32_t sx = (x0 < x1) ? 1 : -1;
	int32_t sy = (y0 < y1) ? 1 : -1;
	int32_t err = dx + dy;

	// 计算包围盒（屏幕坐标），用于标脏
	int32_t minx = min(x0, x1);
	int32_t miny = min(y0, y1);
	int32_t maxx = max(x0, x1);
	int32_t maxy = max(y0, y1);

	while (1) {
		if (x0 >= 0 && x0 < (int32_t)W &&
		    y0 >= 0 && y0 < (int32_t)H) {
			fb[(uint64_t)(y0 + pan) * W + x0] = color;
		}

		if (x0 == x1 && y0 == y1)
			break;

		int32_t e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}

	// 标脏：线的包围盒（resource 绝对坐标）
	if (maxx >= 0 && minx < (int32_t)W &&
	    maxy >= 0 && miny < (int32_t)H) {
		uint32_t cx0 = max(minx, 0);
		uint32_t cy0 = max(miny, 0) + pan;
		uint32_t cx1 = min(maxx, (int32_t)W - 1);
		uint32_t cy1 = min(maxy, (int32_t)H - 1) + pan;

		mark_dirty(cx0, cy0, cx1 - cx0 + 1, cy1 - cy0 + 1);
	}
}
