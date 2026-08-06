#include <type.h>
#include <virtio.h>
#include <memlayout.h>
#include <spinlock.h>
#include <printk.h>

struct virtio_disk usable_disks[8];
spinlock_t vd_lock = {0};
static void detect_disk();

static void detect_disk()
{

        // 映射后的virtio mmio地址
        for (int i = 0; i < 8; i++)
        {

                if (*R_LEVEL(VIRTIO_MMIO_MAGIC_VALUE_OFFSET, i) != VIRTIO_MMIO_MAGIC)
                {
                        continue;
                }
                usable_disks[i].magic = *R_LEVEL(VIRTIO_MMIO_MAGIC_VALUE_OFFSET, i);

                // version
                usable_disks[i].version = *R_LEVEL(VIRTIO_MMIO_VERSION_OFFSET, i);
                // device id
                usable_disks[i].device_id = *R_LEVEL(VIRTIO_MMIO_DEVICE_ID_OFFSET, i);
                usable_disks[i].vendor = *R_LEVEL(VIRTIO_MMIO_VENDOR_ID_OFFSET, i);
                usable_disks[i].base_addr = GET_VIR_BASE(i);

                // 说明支持特性协商
                if (usable_disks[i].version == 2)
                {
                        // HIGH 32
                        *R_LEVEL(VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, i) = 1;
                        usable_disks[i].device_features = *R_LEVEL(VIRTIO_MMIO_DEVICE_FEATURES_OFFSET, i) << 32;

                        // LOW 32
                        *R_LEVEL(VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET, i) = 0;
                        usable_disks[i].device_features |= *R_LEVEL(VIRTIO_MMIO_DEVICE_FEATURES_OFFSET, i);
                }
        }
}

void init_virtio_disk()
{
        init_spinlock(&vd_lock);
        detect_disk();

        for (int i = 0; i < 7; i++)
        {
                // if (usable_disks[i].device_id == 2)

                printk("Disk %d: magic=%#x, version=%d, device_id=%d, vendor=%#x, "
                       "features=%#lx, base=%#lx\n",
                       i,
                       usable_disks[i].magic,
                       usable_disks[i].version,
                       usable_disks[i].device_id,
                       usable_disks[i].vendor,
                       usable_disks[i].device_features,
                       usable_disks[i].base_addr);

                // TODO: 初始化队列、协商特性等
                // virtio_disk_init(&usable_disks[i]);
        }
        while (1)
        {
        }
}