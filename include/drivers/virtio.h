#ifndef _INC_VIRTIO_
#define _INC_VIRTIO_
#include "type.h"

// virtio-mmio 寄存器偏移（相对于 MMIO 基地址）
#define VIRTIO_MMIO_MAGICVALUE_OFFSET 0x000          // R  固定 0x74726976
#define VIRTIO_MMIO_VERSION_OFFSET 0x004             // R  应为 2
#define VIRTIO_MMIO_DEVICEID_OFFSET 0x008            // R  设备类型（2=块设备）
#define VIRTIO_MMIO_VENDORID_OFFSET 0x00c            // R  QEMU 是 0x1af4
#define VIRTIO_MMIO_DEVICE_FEATURES_OFFSET 0x010     // R  设备特性（低 32 位）
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET 0x014 // W  翻页：写0读低32位，写1读高32位
#define VIRTIO_MMIO_DRIVER_FEATURES_OFFSET 0x020     // W  驱动接受的特性（低 32 位）
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET 0x024 // W  翻页：写0写低32位，写1写高32位
// 队列配置
#define VIRTIO_MMIO_QUEUE_SEL_OFFSET 0x030     // W  选择队列号（只支持队列0就写0）
#define VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET 0x034 // R  当前选中队列的最大长度（没配置前是0）
#define VIRTIO_MMIO_QUEUE_NUM_OFFSET 0x038     // W  你要设置的队列长度（必须≤上限）
#define VIRTIO_MMIO_QUEUE_READY_OFFSET 0x044   // W  队列配置完成写1
// 通知与中断
#define VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET 0x050     // W  敲门：写队列号通知设备有活干
#define VIRTIO_MMIO_INTERRUPT_STATUS_OFFSET 0x060 // R  中断状态（设备→驱动）
#define VIRTIO_MMIO_INTERRUPT_ACK_OFFSET 0x064    // W  中断确认（驱动→设备）
// 设备状态机
#define VIRTIO_MMIO_STATUS_OFFSET 0x070 // R/W 初始化状态机
// 共享内存物理地址（低32位/高32位分开写）
#define VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET 0x080    // W  描述符表物理地址低32位
#define VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET 0x084   // W  描述符表物理地址高32位
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFFSET 0x090  // W  可用环(avail)物理地址低32位
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFFSET 0x094 // W  可用环(avail)物理地址高32位
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFFSET 0x0a0  // W  已用环(used)物理地址低32位
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFFSET 0x0a4 // W  已用环(used)物理地址高32位
// 设备配置空间（virtio-blk 用不到，但协议里存在）
#define VIRTIO_MMIO_CONFIG_GENERATION_OFFSET 0x0fc // R 配置变化时递增
#define VIRTIO_MMIO_CONFIG_OFFSET 0x100            // R/W 设备专属配置

#define VIRTIO_MMIO_MAGIC 0x74726976

struct virtio_disk
{
        uint32_t magic;
        uint32_t version;
        uint32_t device_type;
        uint16_t vendor;
        uint64_t device_features;
        uint64_t driver_features;
        uint64_t base_addr;
};

// we reco
extern struct virtio_disk usable_disks[8];
extern spinlock_t vd_lock;
void init_virtio_disk();
#endif
