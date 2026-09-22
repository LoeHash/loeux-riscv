#ifndef __INC_GPU_TTY_H__
#define __INC_GPU_TTY_H__
#include <type.h>
#include <tty.h>
#include <virtio_gpu.h>
#define BLINK_INTERVAL 100

// 当前tty相关信息
struct gpu_tty_state {
	struct virtio_gpu_device* gpu;
	uint32_t cur_x;
	uint32_t cur_y;
	uint32_t fg;
	uint32_t bg;
	int cursor_visible;	// 当前是否可见
	int cursor_drawn;	// 光标是否已经画在屏幕上
	uint32_t blink_counter; // 闪烁计时
	int esc_state;		// 0=正常, 1=ESC, 2=ESC[
	char esc_buf[16];
	int esc_len;
};
void gpu_tty_tick();
void init_gpu_tty();
#endif