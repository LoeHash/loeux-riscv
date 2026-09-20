#include <virtio_gpu.h>
#include <test.h>
#include <printk.h>

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