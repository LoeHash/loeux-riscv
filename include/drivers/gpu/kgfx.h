#ifndef _INC_KGFX_H
#define _INC_KGFX_H
#include <kernel/type.h>
#include <drivers/gpu/virtio_gpu.h>


// 提供内核简单绘制图形的接口
void kgfx_put_pixel(struct virtio_gpu_device *gpu, uint32_t x, uint32_t y, uint32_t color);

void kgfx_fill_rect(struct virtio_gpu_device *gpu, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, uint32_t color);
void kgfx_draw_rect(struct virtio_gpu_device *gpu, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, uint32_t color);
void kgfx_draw_line(struct virtio_gpu_device *gpu,
                    int32_t x0, int32_t y0,
                    int32_t x1, int32_t y1, uint32_t color);
void kgfx_draw_char(struct virtio_gpu_device *gpu, char c, uint32_t x, uint32_t y,
                    uint32_t fg, uint32_t bg);
void kgfx_draw_string(struct virtio_gpu_device *gpu, const char *s,
                      uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);
void kgfx_clear(struct virtio_gpu_device *gpu, uint32_t color);

void kgfx_update(struct virtio_gpu_device *gpu);
void init_kgfx(void);
#endif