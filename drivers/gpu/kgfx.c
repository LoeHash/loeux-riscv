#include <drivers/gpu/kgfx.h>
#include <kernel/spinlock.h>
#include <drivers/gpu/virtio_gpu.h>
#include <utils/math.h>
#include <lib.h>
#include <ascii8x16.h>

// 脏区域
static spinlock_t dirty_lock = {0};
static uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static int dirty_valid;
static void mark_dirty_locked(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static void draw_char_raw(struct virtio_gpu_device *gpu, char c,
                          uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);

void init_kgfx(void)
{
        init_spinlock(&dirty_lock);
        dirty_x0 = 0;
        dirty_y0 = 0;
        dirty_x1 = 0;
        dirty_y1 = 0;
        dirty_valid = 0;
}

void kgfx_put_pixel(struct virtio_gpu_device *gpu,
                    uint32_t x, uint32_t y, uint32_t color)
{
        if (gpu == NULL || gpu->fb == NULL)
                return;
        if (x >= gpu->width || y >= gpu->height)
                return;

        // 写像素
        ((uint32_t *)gpu->fb)[y * gpu->width + x] = color;

        mark_dirty(x, y, 1, 1);
}

void kgfx_clear(struct virtio_gpu_device *gpu, uint32_t color)
{
        if (gpu == NULL || gpu->fb == NULL)
                return;

        uint32_t *p = (uint32_t *)gpu->fb;
        uint64_t n = (uint64_t)gpu->width * gpu->height;

        for (uint64_t i = 0; i < n; i++)
                p[i] = color;

        mark_dirty(0, 0, gpu->width, gpu->height);
}

void kgfx_scroll_up(struct virtio_gpu_device *gpu, uint32_t line_h, uint32_t bg)
{
        if (gpu == NULL || gpu->fb == NULL)
                return;
        if (line_h == 0 || line_h > gpu->height)
                return;

        uint32_t W = gpu->width;
        uint32_t H = gpu->height;
        uint32_t *fb = (uint32_t *)gpu->fb;
        uint64_t total_px = (uint64_t)W * H;
        uint64_t line_px  = (uint64_t)W * line_h;

        /*
         * 内容整体上移 line_h 行：源与目的重叠，必须 memmove。
         * 一次整屏滚动只标一次脏，最终由 kgfx_update 单次刷屏。
         */
        memmove(fb, fb + line_px, (total_px - line_px) * sizeof(uint32_t));

        /* 底部新空出的 line_h 行填背景色 */
        uint32_t *bot = fb + (total_px - line_px);
        for (uint64_t i = 0; i < line_px; i++)
                bot[i] = bg;

        mark_dirty(0, 0, W, H);
}

void kgfx_fill_rect(struct virtio_gpu_device *gpu,
                    uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, uint32_t color)
{
        if (gpu == NULL || gpu->fb == NULL)
                return;
        if (w == 0 || h == 0)
                return;

        // 裁剪到屏幕范围
        uint32_t x0 = x;
        uint32_t y0 = y;
        uint32_t x1 = x + w;
        uint32_t y1 = y + h;

        if (x1 > gpu->width)  x1 = gpu->width;
        if (y1 > gpu->height) y1 = gpu->height;
        if (x0 >= x1 || y0 >= y1)
                return;

        // 写像素：无锁
        for (uint32_t row = y0; row < y1; row++) {
                uint32_t *line = (uint32_t *)gpu->fb + row * gpu->width;
                for (uint32_t col = x0; col < x1; col++)
                        line[col] = color;
        }

        mark_dirty(x0, y0, x1 - x0, y1 - y0);
}

void kgfx_draw_rect(struct virtio_gpu_device *gpu,
                    uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, uint32_t color)
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

void kgfx_update(struct virtio_gpu_device *gpu)
{
        if (gpu == NULL)
                return;

        // 锁内快照脏区域并清零
        acquire(&dirty_lock);

        if (!dirty_valid) {
                release(&dirty_lock);
                return;
        }

        uint32_t x0 = dirty_x0;
        uint32_t y0 = dirty_y0;
        uint32_t x1 = dirty_x1;
        uint32_t y1 = dirty_y1;
        dirty_valid = 0;

        release(&dirty_lock);

        // 锁外发命令：慢操作，不占锁
        virtio_gpu_flush(gpu, x0, y0, x1 - x0, y1 - y0);
}



// internal: 直接写 fb，不标脏
static void draw_char_raw(struct virtio_gpu_device *gpu, char c,
                          uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
        const uint8_t *glyph = ascii8x16[(uint8_t)c];
        uint32_t *fb = (uint32_t *)gpu->fb;
        uint32_t W = gpu->width;
        uint32_t H = gpu->height;

        for (int row = 0; row < ASCII8X16_H; row++) {
                uint32_t py = y + row;
                if (py >= H)
                        break;

                uint8_t bits = glyph[row];
                uint32_t *line = fb + py * W;

                for (int col = 0; col < ASCII8X16_W; col++) {
                        uint32_t px = x + col;
                        if (px >= W)
                                break;

                        line[px] = (bits & (0x80 >> col)) ? fg : bg;
                }
        }
}

void kgfx_draw_char(struct virtio_gpu_device *gpu, char c,
                    uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
        if (gpu == NULL || gpu->fb == NULL)
                return;
        if (x >= gpu->width || y >= gpu->height)
                return;

        draw_char_raw(gpu, c, x, y, fg, bg);

        mark_dirty(x, y, ASCII8X16_W, ASCII8X16_H);
}

void kgfx_draw_string(struct virtio_gpu_device *gpu, const char *s,
                      uint32_t x, uint32_t y, uint32_t fg, uint32_t bg)
{
        if (gpu == NULL || gpu->fb == NULL || s == NULL)
                return;
        if (x >= gpu->width || y >= gpu->height)
                return;

        uint32_t start_x = x;

        while (*s) {
                if (x + ASCII8X16_W > gpu->width)
                        break;   // 超出右边界，停止

                draw_char_raw(gpu, *s, x, y, fg, bg);
                x += ASCII8X16_W;
                s++;
        }

        if (x > start_x)
                mark_dirty(start_x, y, x - start_x, ASCII8X16_H);
}

static void mark_dirty_locked(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
        if (w == 0 || h == 0)
                return;

        uint32_t x1 = x + w;
        uint32_t y1 = y + h;

        if (!dirty_valid) {
                dirty_x0 = x;  dirty_y0 = y;
                dirty_x1 = x1; dirty_y1 = y1;
                dirty_valid = 1;
                return;
        }

        dirty_x0 = min(dirty_x0, x);
        dirty_y0 = min(dirty_y0, y);
        dirty_x1 = max(dirty_x1, x1);
        dirty_y1 = max(dirty_y1, y1);
}

// 标记脏区域
static void mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
        acquire(&dirty_lock);
        mark_dirty_locked(x, y, w, h);
        release(&dirty_lock);
}

void kgfx_draw_line(struct virtio_gpu_device *gpu,
                    int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1, uint32_t color)
{
        if (gpu == NULL || gpu->fb == NULL)
                return;

        uint32_t W = gpu->width;
        uint32_t H = gpu->height;
        uint32_t *fb = (uint32_t *)gpu->fb;

        int32_t dx =  abs(x1 - x0);
        int32_t dy = -abs(y1 - y0);
        int32_t sx = (x0 < x1) ? 1 : -1;
        int32_t sy = (y0 < y1) ? 1 : -1;
        int32_t err = dx + dy;

        // 计算包围盒，用于标脏
        int32_t minx = min(x0, x1);
        int32_t miny = min(y0, y1);
        int32_t maxx = max(x0, x1);
        int32_t maxy = max(y0, y1);

        while (1) {
                if (x0 >= 0 && x0 < (int32_t)W &&
                    y0 >= 0 && y0 < (int32_t)H) {
                        fb[y0 * W + x0] = color;
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

        // 标脏：线的包围盒
        if (maxx >= 0 && minx < (int32_t)W &&
            maxy >= 0 && miny < (int32_t)H) {
                uint32_t cx0 = max(minx, 0);
                uint32_t cy0 = max(miny, 0);
                uint32_t cx1 = min(maxx, (int32_t)W - 1);
                uint32_t cy1 = min(maxy, (int32_t)H - 1);

                mark_dirty(cx0, cy0, cx1 - cx0 + 1, cy1 - cy0 + 1);
        }
}