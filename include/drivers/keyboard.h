// keyboard.h
#ifndef _INC_KEYBOARD_H_
#define _INC_KEYBOARD_H_

#include <type.h>
#include <virtio_mmio.h>

#define VIRTIO_KEYBOARD_DEVICE_ID 18

// 事件类型
#define VIRTIO_INPUT_EV_SYN 0
#define VIRTIO_INPUT_EV_KEY 1

// 键码
#define KEY_ESC        1
#define KEY_1          2
#define KEY_0         11
#define KEY_BACKSPACE 14
#define KEY_TAB       15
#define KEY_Q         16
#define KEY_ENTER     28
#define KEY_LEFTSHIFT 42
#define KEY_Z         44
#define KEY_SPACE     57

// 队列 / 缓冲区
#define VIRTIO_KEYBOARD_MAX_QUEUE_NUM 8
#define INPUT_EVENT_BUFS              8
#define INPUT_CHAR_BUF_SIZE           256

struct virtio_input_event {
	uint16_t type;
	uint16_t code;
	uint32_t value;
} __attribute__((packed));

struct virtio_input_device {
	uintptr_t mmio_base;
	struct virtqueue_n *eventq;
	struct virtqueue_n *statusq;
	int shift;
        uint16_t last_used_idx;
	char buf[INPUT_CHAR_BUF_SIZE];
	int head;
	int tail;
	int initialized;
	struct virtio_input_device *next;
};

void init_keyboard(void);
int  keyboard_getchar(char *out);
int  keyboard_has_input(void);

#endif