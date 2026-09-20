#ifndef _INC_VIRTIO_GPU_H__
#define _INC_VIRTIO_GPU_H__
#include <type.h>

#define VIRTIO_GPU_MAX_QUEUE_NUM 8

// ---- VirtIO GPU 命令码 ----
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100  // 查询显示信息
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101  // 创建 2D 资源
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103  // 设置扫描输出
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104  // 刷新资源到屏幕
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106  // 绑定 guest 内存到资源

// 光标相关
#define VIRTIO_GPU_CMD_UPDATE_CURSOR           0x0300  // 更新光标（位置+图像）
#define VIRTIO_GPU_CMD_MOVE_CURSOR             0x0301  // 仅移动光标

// VirtIO GPU 响应码
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100  // 成功，无额外数据
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101  // 成功，附带显示信息

// 像素格式
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM       1  // 32bpp BGRA
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM       2  // 32bpp BGRX（忽略 alpha）

// GPU 专属特性
#define VIRTIO_GPU_F_VIRGL                     (1 << 0)  // 3D 加速（virgl）
#define VIRTIO_GPU_F_EDID                      (1 << 1)  // 读显示器 EDID

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

struct virtio_gpu_ctrl_hdr {
    uint32_t type;      // 命令类型
    uint32_t flags;     // 标志   0
    uint64_t fence_id;  // 同步用  0
    uint32_t ctx_id;    // 3D 用，2D 填 0
    uint32_t padding;
} __attribute__((packed));


struct virtio_gpu_rect {
    uint32_t x;       // 左上角 x
    uint32_t y;       // 左上角 y
    uint32_t width;   // 宽
    uint32_t height;  // 高
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;  // 分辨率
    uint32_t enabled;          // 是否启用
    uint32_t flags;            // 标志
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;           // 24 字节响应头
    struct virtio_gpu_display_one pmodes[16]; // 最多 16 个 scanout
} __attribute__((packed));

void init_virtio_gpu();
#endif