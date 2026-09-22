#ifndef _INC_KGFX_H
#define _INC_KGFX_H
#include <kernel/type.h>
#include <drivers/gpu/virtio_gpu.h>

// 提供内核简单绘制图形的接口
void kgfx_put_pixel(struct virtio_gpu_device* gpu,
		    uint32_t x,
		    uint32_t y,
		    uint32_t color);

void kgfx_fill_rect(struct virtio_gpu_device* gpu,
		    uint32_t x,
		    uint32_t y,
		    uint32_t w,
		    uint32_t h,
		    uint32_t color);
void kgfx_draw_rect(struct virtio_gpu_device* gpu,
		    uint32_t x,
		    uint32_t y,
		    uint32_t w,
		    uint32_t h,
		    uint32_t color);
void kgfx_draw_line(struct virtio_gpu_device* gpu,
		    int32_t x0,
		    int32_t y0,
		    int32_t x1,
		    int32_t y1,
		    uint32_t color);
void kgfx_draw_char(struct virtio_gpu_device* gpu,
		    char c,
		    uint32_t x,
		    uint32_t y,
		    uint32_t fg,
		    uint32_t bg);
void kgfx_draw_string(struct virtio_gpu_device* gpu,
		      const char* s,
		      uint32_t x,
		      uint32_t y,
		      uint32_t fg,
		      uint32_t bg);
void kgfx_clear(struct virtio_gpu_device* gpu, uint32_t color);

/*
 * 终端滚动：内容整体上移 line_h 像素行，顶部 line_h 行滚出，
 * 底部新出现的 line_h 行填 bg。用于保留终端历史（代替清屏重来）。
 */
void kgfx_scroll_up(struct virtio_gpu_device* gpu,
		    uint32_t line_h,
		    uint32_t bg);

/*
 * 标记脏区需要上屏（轻量，不发 virtio 命令）。
 * 真正的刷屏由时钟节拍统一完成（kgfx_timer_tick），
 * 从而把 10ms 内的所有绘制（包括连续滚屏的全屏脏区）
 * 合并成一次 TRANSFER_TO_HOST_2D + RESOURCE_FLUSH。
 */
void kgfx_update(struct virtio_gpu_device* gpu);

/* 立即把累积的脏区刷到屏幕（如：tty 即将阻塞等输入前） */
void kgfx_flush_now(void);

/* 时钟节拍回调：有待刷内容就刷一次（100Hz 驱动） */
void kgfx_timer_tick(void);

/* 注册当前屏幕设备（单屏），时钟刷新需要它 */
void kgfx_attach(struct virtio_gpu_device* gpu);

void init_kgfx(void);

// 高速批量路径gpu_tty putc，不会标记脏读

/* 不标脏的 fill_rect：调用者负责 kgfx_mark_dirty_screen */
void kgfx_fill_rect_nodirty(struct virtio_gpu_device* gpu,
			    uint32_t x,
			    uint32_t y,
			    uint32_t w,
			    uint32_t h,
			    uint32_t color);

/* 不标脏的 draw_char：调用者负责 kgfx_mark_dirty_screen */
void kgfx_draw_char_nodirty(struct virtio_gpu_device* gpu,
			    char c,
			    uint32_t x,
			    uint32_t y,
			    uint32_t fg,
			    uint32_t bg);

/* 标脏（屏幕坐标，内部自动加 pan 偏移转 resource 绝对坐标） */
void kgfx_mark_dirty_screen(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#endif
