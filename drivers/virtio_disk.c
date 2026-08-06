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
        uint32_t *p;
        uint64_t base;

        // 映射后的virtio mmio地址
        for (int i = 0; i < 8; i++)
        {
                base = MMIO_VIRTIO_OFFEST + i * 4096;
                p = (uint32_t *)base;

                if (*p != VIRTIO_MMIO_MAGIC)
                {
                        continue;
                }
                usable_disks[i].magic = *p + i;

                // version
                p = (uint32_t *)(base + VIRTIO_MMIO_VERSION_OFFSET);
                usable_disks[i].version = *p;

                p = (uint32_t *)(base + VIRTIO_MMIO_DEVICEID_OFFSET);
                if (*p == 2)
                {
                        usable_disks[i].device_type = 2;
                        usable_disks[i].base_addr = base;
                        usable_disks[i].vendor =
                            *((uint16_t *)(base + VIRTIO_MMIO_VENDORID_OFFSET));

                        // HIGH 32
                        *((uint32_t *)(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET)) = 1;
                        usable_disks[i].device_features =
                            ((uint64_t)*((uint32_t *)(base + VIRTIO_MMIO_DEVICE_FEATURES_OFFSET))) << 32;

                        // LOW 32
                        *((uint32_t *)(base + VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFFSET)) = 0;
                        usable_disks[i].device_features |=
                            (*((uint32_t *)(base + VIRTIO_MMIO_DEVICE_FEATURES_OFFSET)));
                }
        }
}

void init_virtio_disk()
{
        init_spinlock(&vd_lock);
        detect_disk();

        for (int i = 0; i < 7; i++)
        {
                if (usable_disks[i].device_type == 2)
                {
                        printk("Disk %d: magic=%#x, version=%d, type=%d, vendor=%#x, "
                               "features=%#lx, base=%#lx\n",
                               i,
                               usable_disks[i].magic,
                               usable_disks[i].version,
                               usable_disks[i].device_type,
                               usable_disks[i].vendor,
                               usable_disks[i].device_features,
                               usable_disks[i].base_addr);

                        // TODO: 初始化队列、协商特性等
                        // virtio_disk_init(&usable_disks[i]);
                }
        }
        while (1)
        {
        }
}