#ifndef _INC_VIRTIO_GPU_H__
#define _INC_VIRTIO_GPU_H__
#include <type.h>

struct virtio_gpu_device {
    uintptr_t mmio_base;              // 设备的 MMIO 基地址，操作寄存器用
    struct virtqueue_n *controlq;     // controlq 队列，发命令用
    struct virtqueue_n *cursorq;      // cursorq 队列，光标用

    uint32_t resource_id;             // framebuffer 资源 ID
    uint32_t cursor_resource_id;      // 光标图像资源 ID
    uint32_t width;                   // 分辨率宽
    uint32_t height;                  // 分辨率高

    void *fb;                         // framebuffer 虚拟地址，写像素用
    phys_addr_t fb_phy;               // framebuffer 物理地址，attach 时填给设备
    uint64_t fb_size;                 // framebuffer 大小

    void *cursor_img;                 // 光标图像虚拟地址
    phys_addr_t cursor_img_phy;       // 光标图像物理地址
    uint32_t cursor_w;                // 光标宽
    uint32_t cursor_h;                // 光标高

    uint32_t next_fence;              // 自增计数器，同步用
    uint32_t scanout_id;              // SET_SCANOUT 用，一般 0
    int initialized;                  // 初始化标志

    struct virtio_gpu_device *next;
};

#endif