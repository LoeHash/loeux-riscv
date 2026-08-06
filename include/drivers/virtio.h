#ifndef _INC_VIRTIO_
#define _INC_VIRTIO_
#include "type.h"

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
#define VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET 0x0A0
#define VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET 0x0A4
#define VIRTIO_MMIO_STATUS_OFFSET 0x070

// VirtIO 状态
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8

#define VIRTIO_MMIO_MAGIC 0x74726976

#define GET_VIR_BASE(level) (uint64_t)((MMIO_VIRTIO_OFFEST + (4096 * level)))
#define R_LEVEL(ofs, level) ((volatile uint32_t *)(GET_VIR_BASE(level) + ofs))

struct virtio_disk
{
        uint32_t magic;
        uint32_t version;
        uint32_t device_id;
        uint32_t vendor;
        uint64_t device_features;
        uint64_t driver_features;
        uint64_t base_addr;
};

// we reco
extern struct virtio_disk usable_disks[8];
extern spinlock_t vd_lock;
void init_virtio_disk();
#endif
