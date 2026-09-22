#ifndef _INC_VIRTIO_
#define _INC_VIRTIO_
#include "type.h"
#include "block_device.h"
#include "memory.h"
#define VIRTIO_QUEUE_INIT_ERROR_VQ_IS_NULL -1
#define VIRTIO_QUEUE_INIT_ERROR_VQ_DESC_TOO_LONG -2

#define VIRTIO_MMIO_COUNT  8

#define VIRTIO_MMIO_MAGIC_VALUE_OFFSET 0x000 // 0x74726976
#define VIRTIO_MMIO_VERSION_OFFSET 0x004     // 应该为 2
#define VIRTIO_MMIO_DEVICE_ID_OFFSET 0x008   // 2 = 块设备
#define VIRTIO_MMIO_VENDOR_ID_OFFSET 0x00C   // 0x554D4551
#define VIRTIO_MMIO_DEVICE_FEATURES_OFFSET 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_OFFSET 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET 0x024
#define VIRTIO_MMIO_QUEUE_SEL_OFFSET 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX_OFFSET 0x034
#define VIRTIO_MMIO_QUEUE_NUM_OFFSET 0x038
#define VIRTIO_MMIO_QUEUE_READY_OFFSET 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET 0x050
#define VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET 0x0A0
#define VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET 0x0A4
#define VIRTIO_MMIO_STATUS_OFFSET 0x070
#define VIRTIO_MMIO_CONFIG_CAPACITY_LOW_OFFSET 0x100
#define VIRTIO_MMIO_CONFIG_CAPACITY_HIGH_OFFSET 0x104
#define VIRTIO_MMIO_CONFIG_SIZE_MAX_OFFSET 0x108
#define VIRTIO_MMIO_CONFIG_SEG_MAX_OFFSET 0x10C

// disk things
#define DISK_SECTOR_SIZE 512
#define VIRTIO_DISK_DEVICE_ID 2 // 块设备
#define VIRTIO_GPU_DEVICE_ID 16 // gpu
#define VIRTIO_KEYBOARD_DEVICE_ID 18 // keyboard
#define SECTOR_SIZE_TO_KB(capacity) ((capacity) * DISK_SECTOR_SIZE / 1024)
#define SECTOR_SIZE_TO_MB(capacity) ((capacity) * DISK_SECTOR_SIZE / 1024 / 1024)
#define SECTOR_SIZE_TO_GB(capacity) ((capacity) * DISK_SECTOR_SIZE / 1024 / 1024 / 1024)

// desc flags
#define VRING_DESC_F_NEXT (1 << 0)     // 有后续描述符
#define VRING_DESC_F_WRITE (1 << 1)    // 设备可写（否则设备只读）
#define VRING_DESC_F_INDIRECT (1 << 2) // 间接描述符表

// VirtIO 状态
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_MMIO_MAGIC 0x74726976
#define GET_VIR_BASE(level) (uint64_t)((MMIO_VIRTIO_OFFEST + (4096 * level)))
#define R_LEVEL(ofs, level) ((volatile uint32_t *)(GET_VIR_BASE(level) + ofs))

#define QUEUE_SIZE 56
#define END_OF_NEXT_FLAG 0xffff

// 状态码定义
#define VIRTIO_BLK_S_OK 0     // 成功
#define VIRTIO_BLK_S_IOERR 1  // I/O错误
#define VIRTIO_BLK_S_UNSUPP 2 // 不支持的操作

// type 操作
#define VIRTIO_BLK_T_IN 0            // 读操作 - 从设备读取数据到缓冲区
#define VIRTIO_BLK_T_OUT 1           // 写操作 - 将缓冲区数据写入设备
#define VIRTIO_BLK_T_FLUSH 4         // 刷新操作 - 将所有缓存数据写入持久存储
#define VIRTIO_BLK_T_GET_ID 8        // 获取设备ID - 读取设备标识字符串 (通常20字节)
#define VIRTIO_BLK_T_DISCARD 11      // 丢弃操作 - 通知设备指定范围的数据已无效 (类似TRIM)
#define VIRTIO_BLK_T_WRITE_ZEROES 13 // 写零操作 - 向指定范围写入全零数据
#define VIRTIO_BLK_T_SECURE_ERASE 14 // 安全擦除 - 密码擦除

#define GET_LOW_32(x) ((uint32_t)((x) & 0xFFFFFFFFULL))
#define GET_HIGH_32(x) ((uint32_t)((x) >> 32))

struct virtio_blk_req
{
        uint32_t type;
        uint32_t ioprio; // 保留
        uint64_t sector;
};

struct virtio_blk_config
{
        uint64_t capacity;
        uint32_t size_max;
        uint32_t seg_max;
};

struct virtio_blk_disk
{
        uint32_t initialized;
        uint32_t magic;
        uint32_t version;
        uint32_t device_id;
        uint32_t vendor;
        uint64_t device_features;
        uint64_t driver_features;
        uint64_t base_addr;
        spinlock_t vbd_lock;
        struct virtio_blk_config blk_config;
};

struct virtq_desc_n {
    uint64_t addr;   // 缓冲区物理地址
    uint32_t len;    // 缓冲区长度
    uint16_t flags;  // 标志
    uint16_t next;   // 下一个描述符下标
} __attribute__((packed));

// 与virtio-blk设备沟通需要三个数据结构来维护
struct virtq_desc
{
        uint64_t addr; // 内存物理地址
        uint32_t len;
        uint16_t flags; // bit0: NEXT, bit1: WRITE
        uint16_t next;
} __attribute__((packed));

struct virtq_avail
{
        uint16_t flags;
        uint16_t idx;              // 驱动已投递的请求数
        uint16_t ring[QUEUE_SIZE]; // 描述符索引
} __attribute__((packed));

struct virtq_avail_n {
    uint16_t flags;
    uint16_t idx;      // 驱动下一个要写的位置
    uint16_t ring[];   // 存 descriptor 下标
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;    // 处理完的描述符链头下标
    uint32_t len;   // 设备实际写了多少字节
} __attribute__((packed));

/*
 * virtio split vring 的 used ring 布局
 *   offset 0: u16 flags
 *   offset 2: u16 idx
 *   offset 4: used_elem ring[QueueSize] 
 * 每个 used_elem 为 { u32 id; u32 len; }
 * 千万不要在 idx 与 ring 之间插 padding，否则读 ring[] 会整体后移 4 字节，
 * 把上一个元素的 len 误读成下一个元素的 id 键盘驱动因此把 len=8 当成
 * desc id=8 回填 avail，触发 QEMU "Guest says index 8 is available"。
 */
struct virtq_used_n {
    uint16_t flags;
    uint16_t idx;                    // 设备已完成的请求数
    struct virtq_used_elem ring[];  // offset 4
} __attribute__((packed));

struct virtq_used
{
        uint16_t flags;
        uint16_t idx; // 设备已完成的请求数
        struct
        {
                uint32_t id;
                uint32_t len;
        } ring[QUEUE_SIZE];
} __attribute__((packed));

struct virtqueue_n
{
        uintptr_t mmio_base;    // base地址
        uint32_t queue_idx;     // 驱动填写

        int irq;
        // 驱动用
        volatile struct virtq_desc_n *desc_start;
        volatile struct virtq_avail_n *avail_start;
        volatile struct virtq_used_n *used_start;
        
        // 设备用 phy
        phys_addr_t desc_phy;
        phys_addr_t avail_phy;
        phys_addr_t used_phy;

        // 空闲位图
        // 0 free, 1 used
        volatile uint64_t free_desc_bit_map;
        spinlock_t fdbm_lk;

        // 队列大小
        int queue_size;

        spinlock_t vq_lock;
} ;


struct virtqueue
{
        // 中断号
        int irq;

        // mmio_base
        uintptr_t mmio_base;
        uint32_t queue_idx;

        // 驱动用
        struct virtq_desc *desc_start;
        struct virtq_avail *avail_start;
        struct virtq_used *used_start;
        
        // 设备用 phy
        phys_addr_t desc_phy;
        phys_addr_t avail_phy;
        phys_addr_t used_phy;

        // 空闲位图
        // 0 free, 1 used
        volatile uint64_t free_desc_bit_map;
        spinlock_t fdbm_lk;

        // 队列大小
        int queue_size;

        spinlock_t vq_lock;
} ;

// we reco
extern struct virtio_blk_disk usable_disks[8];
extern spinlock_t vd_alloc_lock;
extern spinlock_t vd_free_lock;
extern volatile uint64_t usable_device_count;
extern struct virtqueue vq;
extern struct block_driver virtio_block_driver;
extern struct block_device virtio_block_device;
void init_virtio_disk();
int alloc_desc(int n);
void free_desc(struct virtq_desc *chain_head);
uint32_t virtio_disk_rw_sync(
    struct virtio_blk_disk *selected_desk,
    struct virtio_blk_req *req,
    void *buf,
    uint32_t bytes,
    uint8_t *status);



static inline void b8_write(uint64_t addr, uint8_t data)
{
        *((volatile uint8_t *)addr) = data;
        MEMORY_FENCE;
}

static inline void b16_write(uint64_t addr, uint16_t data)
{
        *((volatile uint16_t *)addr) = data;
        MEMORY_FENCE;
}

static inline void b32_write(uint64_t addr, uint32_t data)
{
        *((volatile uint32_t *)addr) = data;
        MEMORY_FENCE;
}

static inline void b64_write(uint64_t addr, uint64_t data)
{
        *((volatile uint64_t *)addr) = data;
        MEMORY_FENCE;
}

static inline uint8_t b8_read(uint64_t addr)
{
        uint8_t data = *((volatile uint8_t *)addr);
        MEMORY_FENCE;
        return data;
}

static inline uint16_t b16_read(uint64_t addr)
{
        uint16_t data = *((volatile uint16_t *)addr);
        MEMORY_FENCE;
        return data;
}

static inline uint32_t b32_read(uint64_t addr)
{
        uint32_t data = *((volatile uint32_t *)addr);
        MEMORY_FENCE;
        return data;
}

static inline uint64_t b64_read(uint64_t addr)
{
        uint64_t data = *((volatile uint64_t *)addr);
        MEMORY_FENCE;
        return data;
}


void dump_sector_n(struct virtio_blk_disk *disk, int n);

int virtio_blk_read(void *dev, uint64_t sector, void *buf);
int virtio_blk_write(void *dev, uint64_t sector, const void *buf);
uint64_t virtio_blk_sector_count(void *dev);
struct virtqueue_n *alloc_virtqueue(int queue_size);
int init_virtqueue(struct virtqueue_n *vq, uint32_t start_idx);
int virtqueue_send(struct virtqueue_n *vq,
                   void *cmd,  uint32_t cmd_len,
                   void *resp, uint32_t resp_len);

/*
 * 批量提交两条命令到同一 vring，只 NOTIFY 一次。
 * 把 T2D + FLUSH 合并为一趟 QEMU 同步处理，命令往返减半。
 * 返回 0 成功，-1 失败。
 */
int virtqueue_send_dual(struct virtqueue_n *vq,
    void *cmd1, uint32_t cmd1_len, void *resp1, uint32_t resp1_len,
    void *cmd2, uint32_t cmd2_len, void *resp2, uint32_t resp2_len);
#endif
