#include <virtio_gpu.h>
#include <test.h>
#include <drivers/gpu/kgfx.h>
#include <printk.h>
static void stupid_delay(uint32_t ticks);

void virtio_gpu_test_draw(struct virtio_gpu_device *gpu)
{
        if (gpu == NULL || gpu->fb == NULL) {
                printk("test_draw: gpu or fb is NULL\n");
                return;
        }

        uint32_t *pixels = (uint32_t *)gpu->fb;
        uint32_t  w = gpu->width;
        uint32_t  h = gpu->height;

        printk("test_draw: fb=%0#lx phy=%0#lx size=%lu w=%u h=%u\n",
               (uint64_t)gpu->fb, gpu->fb_phy, gpu->fb_size, w, h);

        /*
         * 像素格式 B8G8R8A8，小端内存序 [B,G,R,A]，
         * 高字节是 alpha：必须给 0xFF 不透明，否则部分 QEMU
         * 图形后端会按透明像素与黑色背景混合，看起来仍是黑屏。
         */
        // 顶部 1/3 红色
        for (uint32_t y = 0; y < h / 3; y++)
                for (uint32_t x = 0; x < w; x++)
                        pixels[y * w + x] = 0xFFFF0000;

        // 中间 1/3 绿色
        for (uint32_t y = h / 3; y < 2 * h / 3; y++)
                for (uint32_t x = 0; x < w; x++)
                        pixels[y * w + x] = 0xFF00FF00;

        // 底部 1/3 蓝色
        for (uint32_t y = 2 * h / 3; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                        pixels[y * w + x] = 0xFF0000FF;

        printk("fb[0]=%0#x fb[mid]=%0#x fb[last]=%0#x\n",
               pixels[0], pixels[w * h / 2], pixels[w * h - 1]);

        int r = virtio_gpu_flush(gpu, 0, 0, w, h);
        printk("flush returned %d\n", r);
}

void kgfx_test_rect(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);                          // 黑底
        kgfx_fill_rect(gpu, 100, 100, 200, 150, 0xFFFF0000);  // 红色实心矩形
        kgfx_draw_rect(gpu, 400, 100, 300, 200, 0xFF00FF00);  // 绿色边框矩形
        kgfx_update(gpu);                                                       // 刷新脏区域
}

void kgfx_test_text(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);
        kgfx_draw_string(gpu, "Hello, Loeux!", 100, 100, 0xFFFFFFFF, 0xFF000000);
        kgfx_draw_string(gpu, "0123456789", 100, 130, 0xFFFFFF00, 0xFF000000);
        kgfx_update(gpu);
}


void kgfx_test_line(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);

        // 画一个星形
        int cx = 640, cy = 400, r = 200;
        kgfx_draw_line(gpu, cx, cy - r, cx - r, cy + r, 0xFFFFFFFF);
        kgfx_draw_line(gpu, cx - r, cy + r, cx + r, cy + r, 0xFFFFFFFF);
        kgfx_draw_line(gpu, cx + r, cy + r, cx, cy - r, 0xFFFFFFFF);

        // 对角线
        kgfx_draw_line(gpu, 0, 0, 1279, 799, 0xFFFF0000);

        kgfx_update(gpu);
}

// 综合测试：把所有可见字符、各种图形都画出来
#include <kgfx.h>
#include <ascii8x16.h>

void kgfx_test_all_chars(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);

        // 打印所有可见 ASCII 字符（0x20 ~ 0x7E）
        uint32_t x = 8;
        uint32_t y = 8;
        uint32_t col = 0;

        for (int c = 0x20; c <= 0x7E; c++) {
                kgfx_draw_char(gpu, (char)c, x, y, 0xFFFFFFFF, 0xFF000000);
                x += ASCII8X16_W;
                col++;
                if (col >= 60) {
                        col = 0;
                        x = 8;
                        y += ASCII8X16_H + 2;
                }
        }

        kgfx_update(gpu);
}

void kgfx_test_color_bars(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);

        // 8 条色带：红、绿、蓝、黄、紫、青、白、灰
        uint32_t colors[] = {
                0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00,
                0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF, 0xFF808080,
        };

        uint32_t bar_h = gpu->height / 8;
        for (int i = 0; i < 8; i++) {
                kgfx_fill_rect(gpu, 0, i * bar_h, gpu->width, bar_h, colors[i]);
        }

        kgfx_update(gpu);
}

void kgfx_test_rects(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);

        // 一堆不同大小、颜色的矩形边框
        uint32_t colors[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF };

        for (int i = 0; i < 20; i++) {
                uint32_t w = 50 + i * 20;
                uint32_t h = 30 + i * 15;
                uint32_t x = (gpu->width  - w) / 2 + i * 5;
                uint32_t y = (gpu->height - h) / 2 + i * 5;
                kgfx_draw_rect(gpu, x, y, w, h, colors[i % 5]);
        }

        kgfx_update(gpu);
}

void kgfx_test_lines(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);

        // 扇形射线
        int cx = gpu->width / 2;
        int cy = gpu->height / 2;
        int r  = 300;

        for (int a = 0; a < 360; a += 15) {
                int x1 = cx + r * a / 60;
                int y1 = cy + r * a / 60;
                kgfx_draw_line(gpu, cx, cy, x1, y1, 0xFFFFFFFF);
        }

        // 横竖网格
        for (int x = 0; x < (int)gpu->width; x += 64)
                kgfx_draw_line(gpu, x, 0, x, gpu->height - 1, 0xFF404040);
        for (int y = 0; y < (int)gpu->height; y += 64)
                kgfx_draw_line(gpu, 0, y, gpu->width - 1, y, 0xFF404040);

        kgfx_update(gpu);
}

void kgfx_test_text_lines(struct virtio_gpu_device *gpu)
{
        kgfx_clear(gpu, 0xFF000000);

        uint32_t y = 8;
        uint32_t colors[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFFFFFF };

        const char *msgs[] = {
                "Line 01: Hello Loeux!",
                "Line 02: 0123456789",
                "Line 03: !@#$%^&*()_+-=[]{}|;':\",./<>?",
                "Line 04: The quick brown fox jumps over the lazy dog",
                "Line 05: ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                "Line 06: abcdefghijklmnopqrstuvwxyz",
                "Line 07: This is a test of the kernel tty.",
                "Line 08: 1280x800 framebuffer",
        };

        for (int i = 0; i < 8; i++) {
                kgfx_draw_string(gpu, msgs[i], 16, y, colors[i % 5], 0xFF000000);
                y += ASCII8X16_H + 4;
        }

        kgfx_update(gpu);
}

// 综合测试，依次跑所有
void kgfx_test_all(struct virtio_gpu_device *gpu)
{
        kgfx_test_all_chars(gpu);
        stupid_delay(300000000);
        kgfx_test_color_bars(gpu);
        stupid_delay(300000000);
        kgfx_test_rects(gpu);
        stupid_delay(300000000);
        kgfx_test_lines(gpu);
        stupid_delay(300000000);
        kgfx_test_text_lines(gpu);
        stupid_delay(300000000);
}

static void stupid_delay(uint32_t ticks){
        while (ticks--) {
        }
}