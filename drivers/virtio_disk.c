#include <type.h>
#include <virtio.h>
#include <lib.h>
#include <memory.h>
#include <memlayout.h>
#include <panic.h>
#include <spinlock.h>
#include <printk.h>

// virtio queue 队列
struct virtqueue vq = {0};

struct virtio_disk usable_disks[8];
volatile uint64_t usable_device_count = 0;
spinlock_t vd_alloc_lock = {0};
spinlock_t vd_free_lock = {0};
static void detect_disk();
static void init_virtio_queue();

/// @brief 同步从设备中读取bytes个字节
/// @param req          请求头
/// @param buf          缓冲区
/// @param bytes        字节数，为512整数倍
/// @param status       状态会写
/// @return     实际读取/写入的字节数
uint32_t virtio_disk_rw_sync(
    struct virtio_blk_req *req,
    void *buf,
    uint32_t bytes,
    uint8_t *status)
{
}

uint32_t do_read()
{
}

uint32_t do_write()
{
}

static void init_virtio_queue()
{
        vq.desc_start = alloc_page();
        vq.avail_start = alloc_page();
        vq.used_start = alloc_page();
        memset(vq.desc_start, 0, 4096);
        memset(vq.avail_start, 0, 4096);
        memset(vq.used_start, 0, 4096);

        // 恒等映射
        vq.desc_phy = vq.desc_start;
        vq.avail_phy = vq.avail_start;
        vq.used_phy = vq.used_start;

        vq.queue_size = QUEUE_SIZE;

        vq.free_desc_bit_map = 0;
        init_spinlock(&vq.fdbm_lk);
}

static void detect_disk()
{

        // 映射后的virtio mmio地址
        for (int i = 0; i < 8; i++)
        {

                if (*R_LEVEL(VIRTIO_MMIO_MAGIC_VALUE_OFFSET, i) != VIRTIO_MMIO_MAGIC)
                {
                        continue;
                }
                if (*R_LEVEL(VIRTIO_MMIO_DEVICE_ID_OFFSET, i) == 0)
                {
                        // empty.
                        continue;
                }

                usable_device_count++;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);

                // magic
                usable_disks[i]
                    .magic = *R_LEVEL(VIRTIO_MMIO_MAGIC_VALUE_OFFSET, i);
                // version
                usable_disks[i].version = *R_LEVEL(VIRTIO_MMIO_VERSION_OFFSET, i);
                // device id
                usable_disks[i].device_id = *R_LEVEL(VIRTIO_MMIO_DEVICE_ID_OFFSET, i);
                // vendor
                usable_disks[i].vendor = *R_LEVEL(VIRTIO_MMIO_VENDOR_ID_OFFSET, i);
                // virtio addr
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

                        // 但我们仅使用基础功能
                        // HIGH 32
                        *R_LEVEL(VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, i) = 1;
                        *R_LEVEL(VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, i) = 0;

                        // LOW 32
                        *R_LEVEL(VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFFSET, i) = 0;
                        *R_LEVEL(VIRTIO_MMIO_DRIVER_FEATURES_OFFSET, i) = 0;

                        usable_disks[i].driver_features = 0;
                }
        }
}

/// @brief 传入decs链头，释放全部，调用者必须确定chain_head是由alloc_desc分配的
/// @param chain_head
void free_desc(struct virtq_desc *chain_head)
{
        if (chain_head == NULL)
        {
                panic(PANIC_ERROR, "virtio_disk.c:free_desc(): chain_head is null!\n");
        }

        struct virtq_desc *desc = chain_head;
        int tmp_idx = 0;

        acquire(&vq.fdbm_lk);
        while (1)
        {
                vq.free_desc_bit_map &= ~(1 << (desc - vq.desc_start));

                // 如果还有下一个
                if ((desc->flags & VIRTQ_DESC_F_NEXT))
                {
                        // 先释放当前desc
                        tmp_idx = desc->next;
                        desc->next = -1;
                        desc->flags = 0;
                        desc = &vq.desc_start[tmp_idx];
                }
                else
                {
                        desc->next = -1;
                        desc->flags = 0;
                        break;
                }
        }
        release(&vq.fdbm_lk);
}

/// @brief 分配空闲的desc链
/// @param n 需要多少个desc
/// @return 链头索引，-1 错误
int alloc_desc(int n)
{
        if (n > QUEUE_SIZE)
        {
                return -1;
        }
        int head_idx = -1, found = 0;
        struct virtq_desc *prev = 0;
        int found_idx[QUEUE_SIZE];

        acquire(&vq.fdbm_lk);
        // 遍历位图
        for (int i = 0; i < QUEUE_SIZE; i++)
        {
                if (vq.free_desc_bit_map & (1 << i))
                {
                        // 被占用
                        continue;
                }

                // 说明是第一次
                if (head_idx == -1)
                {
                        head_idx = i;
                }

                // 没有占用
                // 置bit
                vq.free_desc_bit_map |= (1 << i);

                if (prev == 0)
                {
                        prev = &vq.desc_start[i];
                }
                else
                {
                        // 说明前面已经找到了
                        prev->next = i;
                }

                // 设置desc的flag
                prev->flags |= VIRTQ_DESC_F_NEXT;

                found_idx[found] = i;

                if (++found == n)
                {
                        // 已经找到足够的desc了
                        // 给驱动看的标志
                        vq.desc_start[i].next = END_OF_NEXT_FLAG;

                        // 给设备看的标志
                        vq.desc_start[i].flags = 0;
                        break;
                }
                else
                {
                        // 没找到足够的
                        // 将prev置为当前的desc
                        prev = &vq.desc_start[i];
                }
        }

        // 最后看最终找到的
        // 因为有可能什么也没找到
        // 或不满足要求的数量
        if (found != n && head_idx != -1)
        {
                // 进入到这里
                // 一定是找到的数量不满足n
                // 直接根据found来回滚
                // 释放掉全部，并设置head_idx = -1

                for (int i = 0; i < found; i++)
                {
                        prev = &vq.desc_start[found_idx[i]];
                        prev->flags = 0;
                        prev->next = -1;
                        vq.free_desc_bit_map &= ~(1 << found_idx[i]);
                }

                head_idx = -1;
        }
        release(&vq.fdbm_lk);

        return head_idx;
}

void init_virtio_disk()
{
        init_spinlock(&vd_alloc_lock);
        init_spinlock(&vd_free_lock);
        detect_disk();

        // 队列分配
        init_virtio_queue();

        for (int i = 0; i < usable_device_count; i++)
        {
                printk("Disk %d: magic=%#x, version=%d, device_id=%d, vendor=%#x, "
                       "features=%#lx,driver_features=%#lx base=%#lx\n",
                       i,
                       usable_disks[i].magic,
                       usable_disks[i].version,
                       usable_disks[i].device_id,
                       usable_disks[i].vendor,
                       usable_disks[i].device_features,
                       usable_disks[i].driver_features,
                       usable_disks[i].base_addr);
        }
        while (1)
        {
        }
}