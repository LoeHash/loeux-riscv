#ifndef _INC_VIRTIO_
#define _INC_VIRTIO_
#include "type.h"
#include "block_device.h"
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

// 与virtio-blk设备沟通需要三个数据结构来维护
struct virtq_desc
{
        uint64_t addr; // 内存物理地址
        uint32_t len;
        uint16_t flags; // bit0: NEXT, bit1: WRITE
        uint16_t next;
};

struct virtq_avail
{
        uint16_t flags;
        uint16_t idx;              // 驱动已投递的请求数
        uint16_t ring[QUEUE_SIZE]; // 描述符索引
};

struct virtq_used
{
        uint16_t flags;
        uint16_t idx; // 设备已完成的请求数
        struct
        {
                uint32_t id;
                uint32_t len;
        } ring[QUEUE_SIZE];
};

struct virtqueue
{
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
};

// we reco
extern struct virtio_blk_disk usable_disks[8];
extern spinlock_t vd_alloc_lock;
extern spinlock_t vd_free_lock;
extern volatile uint64_t usable_device_count;
extern struct virtqueue vq;
extern struct block_driver virtio_block_driver;
void init_virtio_disk();
int alloc_desc(int n);
void free_desc(struct virtq_desc *chain_head);
uint32_t virtio_disk_rw_sync(
    struct virtio_blk_disk *selected_desk,
    struct virtio_blk_req *req,
    void *buf,
    uint32_t bytes,
    uint8_t *status);

void b8_write(uint64_t addr, uint8_t data);
void b16_write(uint64_t addr, uint16_t data);
void b64_write(uint64_t addr, uint64_t data);
void b32_write(uint64_t addr, uint32_t data);
void dump_sector_n(struct virtio_blk_disk *disk, int n);

int virtio_blk_read(void *dev, uint64_t sector, void *buf);
int virtio_blk_write(void *dev, uint64_t sector, const void *buf);
uint64_t virtio_blk_sector_count(void *dev);
#endif
