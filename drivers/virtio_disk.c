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
/// @return     实际读取/写入的字节数 -1 on error
uint32_t virtio_disk_rw_sync(
    struct virtio_disk *selected_desk,
    struct virtio_blk_req *req,
    void *buf,
    uint32_t bytes,
    uint8_t *status)
{
        if (bytes % DISK_SECTOR_SIZE != 0)
        {
                return -1;
        }

        // 分配三个 desc 头
        int head_idx = alloc_desc(3);

        while (head_idx == -1)
        {
                // 多种原因导致分配失败
                // 重试
                head_idx = alloc_desc(3);
        }

        int next_tmp_idx = head_idx;
        // 填充第一个desc，请求头
        vq.desc_start[next_tmp_idx].addr = (uint64_t)req;
        vq.desc_start[next_tmp_idx].len = sizeof(struct virtio_blk_req); // 请求头长度
        vq.desc_start[next_tmp_idx].flags |= VRING_DESC_F_NEXT;          // 可读
        next_tmp_idx = vq.desc_start[next_tmp_idx].next;

        // 填充第二个desc, 缓冲区
        // 根据请求头来写
        if (req->type == VIRTIO_BLK_T_IN)
        {
                // 读请求
                // 也就是让设备将数据写入到我们的缓冲区
                // 因此设备flag 加上写
                vq.desc_start[next_tmp_idx].flags |=
                    (VRING_DESC_F_NEXT | VRING_DESC_F_WRITE); // 可读
        }
        else if (req->type == VIRTIO_BLK_T_OUT)
        {
                // 写请求
                // 也就是让设备将缓冲区的数据读取
                // 并写入到设备
                // 因此设备flag 加上写
                vq.desc_start[next_tmp_idx].flags |=
                    (VRING_DESC_F_NEXT);
        }
        vq.desc_start[next_tmp_idx].addr = (uint64_t)buf;
        vq.desc_start[next_tmp_idx].len = bytes; // 请求头长度
        next_tmp_idx = vq.desc_start[next_tmp_idx].next;

        // 第三个desc
        vq.desc_start[next_tmp_idx].addr = (uint64_t)status;
        vq.desc_start[next_tmp_idx].len = 1;
        vq.desc_start[next_tmp_idx].flags = VRING_DESC_F_WRITE;

        // 提交到avail
        vq.avail_start->ring[vq.avail_start->idx % vq.queue_size] = head_idx;
        vq.avail_start->idx++;
        MEMORY_FENCE;

        // 同步等待
        uint16_t last = vq.used_start->idx;

        // 通知设备
        b32_write(selected_desk->base_addr + VIRTIO_MMIO_QUEUE_NOTIFY_OFFSET, 0); // 0 队列有任务

        while (vq.used_start->idx == last)
        {
                asm volatile("wfi");
        }

        // 走到这里，读取或写入完成
        // 根据status判断状态
        if (*status != 0)
        {
                // panic(PANIC_ERROR, "virtio_disk.c:virtio_disk_rw_sync:panic the status: %d\n", *status);

                free_desc(&vq.desc_start[head_idx]);
                return -1;
        }

        // vq.used_start->idx 语义：表示下一次的空位置
        uint32_t used_idx = (vq.used_start->idx - 1) & (vq.queue_size - 1);

        // 成功完成
        // 释放desc
        free_desc(&vq.desc_start[head_idx]);
        // 返回实际的
        // -1 为状态字节
        return vq.used_start->ring[used_idx].len - 1;
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
        vq.desc_phy = (phys_addr_t)vq.desc_start;
        vq.avail_phy = (phys_addr_t)vq.avail_start;
        vq.used_phy = (phys_addr_t)vq.used_start;

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
                        usable_disks[i].device_features = (uint64_t)(*R_LEVEL(VIRTIO_MMIO_DEVICE_FEATURES_OFFSET, i)) << 32;

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
                if ((desc->flags & VRING_DESC_F_NEXT))
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
                prev->flags |= VRING_DESC_F_NEXT;

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
        uint64_t b_addr;
        for (int i = 0; i < usable_device_count; i++)
        {
                b_addr = usable_disks[i].base_addr;
                // 复位
                b32_write(b_addr + VIRTIO_MMIO_STATUS_OFFSET, 0);
                b32_write(b_addr + VIRTIO_MMIO_STATUS_OFFSET, VIRTIO_STATUS_ACKNOWLEDGE);

                b32_write(b_addr + VIRTIO_MMIO_STATUS_OFFSET,
                          VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

                b32_write(b_addr + VIRTIO_MMIO_STATUS_OFFSET,
                          VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

                // 配置desc, avail, used
                // 选择队列
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_SEL_OFFSET, 0); // QueueSel = 0

                // 设置大小
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_NUM_OFFSET, 256); // QueueNum = 256

                // 写地址
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_DESC_LOW_OFFSET, GET_LOW_32(vq.desc_phy));
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_DESC_HIGH_OFFSET, GET_HIGH_32(vq.desc_phy));
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_AVAIL_LOW_OFFSET, GET_LOW_32(vq.avail_phy));
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_AVAIL_HIGH_OFFSET, GET_HIGH_32(vq.avail_phy));
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_USED_LOW_OFFSET, GET_LOW_32(vq.used_phy));
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_USED_HIGH_OFFSET, GET_HIGH_32(vq.used_phy));

                // 标记就绪
                b32_write(b_addr + VIRTIO_MMIO_QUEUE_READY_OFFSET, 1); // QueueReady = 1

                b32_write(b_addr + VIRTIO_MMIO_STATUS_OFFSET,
                          VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

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
}

void b8_write(uint64_t addr, uint8_t data)
{

        *((uint8_t *)addr) = data;
        MEMORY_FENCE;
}

void b16_write(uint64_t addr, uint16_t data)
{

        *((uint16_t *)addr) = data;
        MEMORY_FENCE;
}

void b64_write(uint64_t addr, uint64_t data)
{

        *((uint64_t *)addr) = data;
        MEMORY_FENCE;
}

void b32_write(uint64_t addr, uint32_t data)
{

        *((uint32_t *)addr) = data;
        MEMORY_FENCE;
}

void test_write_verify()
{
        struct virtio_disk *disk = &usable_disks[0];
        struct virtio_blk_req req;
        uint8_t status;
        uint8_t write_buf[512], read_buf[512] = {0};

        // 构造数据
        for (int i = 0; i < 512; i++)
                write_buf[i] = i & 0xFF;
        strcpy((char *)write_buf, "Hello VirtIO!");

        // 写入
        req.type = VIRTIO_BLK_T_OUT;
        req.sector = 0;
        req.ioprio = 0;
        virtio_disk_rw_sync(disk, &req, write_buf, 512, &status);

        // 读回
        req.type = VIRTIO_BLK_T_IN;
        req.sector = 0;
        req.ioprio = 0;
        virtio_disk_rw_sync(disk, &req, read_buf, 512, &status);

        // 验证
        if (memcmp(write_buf, read_buf, 512) == 0)
        {
                printk("✓ Write verified!\n");
        }
        else
        {
                printk("✗ Write failed!\n");
        }
}

void test_virtio_disk_rw_sync()
{
        printk("in..\n");
        struct virtio_disk *disk = &usable_disks[0]; // 使用第一个磁盘
        struct virtio_blk_req req;
        uint8_t status;
        int ret;

        printk("\n========== VirtIO Disk Test Start ==========\n");

        printk("\n[Test 1] Read sector 0 (512 bytes)\n");

        uint8_t read_buf1[512] = {0};
        req.type = VIRTIO_BLK_T_IN;
        req.sector = 0;
        req.ioprio = 0;

        ret = virtio_disk_rw_sync(disk, &req, read_buf1, 512, &status);

        if (ret > 0)
        {
                printk("  ✓ Read success: %d bytes\n", ret);
                printk("  First 16 bytes: ");
                for (int i = 0; i < 16 && i < ret; i++)
                {
                        printk("%0#lx ", read_buf1[i]);
                }
                printk("\n");
        }
        else
        {
                printk("  ✗ Read failed: %d (status=%d)\n", ret, status);
        }

        printk("\n[Test 2] Write sector 0 then read back\n");

        uint8_t write_buf[512];
        uint8_t read_buf2[512] = {0};

        // 准备测试数据
        for (int i = 0; i < 512; i++)
        {
                write_buf[i] = i & 0xFF;
        }
        strcpy((char *)write_buf, "Hello VirtIO Block Device!");

        // 写入
        req.type = VIRTIO_BLK_T_OUT;
        req.sector = 0;
        req.ioprio = 0;

        ret = virtio_disk_rw_sync(disk, &req, write_buf, 512, &status);
        if (ret >= 0)
        {
                printk("  ✓ Write success: %d bytes\n", ret);
        }
        else
        {
                printk("  ✗ Write failed: %d (status=%d)\n", ret, status);
                return;
        }

        // 读回验证
        req.type = VIRTIO_BLK_T_IN;
        req.sector = 0;

        ret = virtio_disk_rw_sync(disk, &req, read_buf2, 512, &status);
        if (ret > 0)
        {
                printk("  ✓ Read back success: %d bytes\n", ret);

                // 验证数据
                if (memcmp(write_buf, read_buf2, 512) == 0)
                {
                        printk("  ✓ Data verification: PASSED\n");
                }
                else
                {
                        printk("  ✗ Data verification: FAILED\n");
                        printk("  Expected first 16 bytes: ");
                        for (int i = 0; i < 16; i++)
                                printk("%0#lx ", write_buf[i]);
                        printk("\n  Actual first 16 bytes:   ");
                        for (int i = 0; i < 16; i++)
                                printk("%0#lx ", read_buf2[i]);
                        printk("\n");
                }
        }
        else
        {
                printk("  ✗ Read back failed: %d (status=%d)\n", ret, status);
        }

        printk("\n[Test 3] Read 8 sectors (4096 bytes) from sector 1\n");

        uint8_t read_buf3[4096] = {0};
        req.type = VIRTIO_BLK_T_IN;
        req.sector = 1;
        req.ioprio = 0;

        ret = virtio_disk_rw_sync(disk, &req, read_buf3, 4096, &status);

        if (ret > 0)
        {
                printk("  ✓ Read success: %d bytes\n", ret);
                if (ret == 4096)
                {
                        printk("  ✓ Full 4KB read completed\n");
                }
                else
                {
                        printk("  ⚠ Partial read: expected 4096, got %d\n", ret);
                }
                printk("  First 16 bytes: ");
                for (int i = 0; i < 16 && i < ret; i++)
                {
                        printk("%0#lx ", read_buf3[i]);
                }
                printk("\n");
        }
        else
        {
                printk("  ✗ Read failed: %d (status=%d)\n", ret, status);
        }

        printk("\n[Test 4] Read last sector\n");

        // 假设磁盘有 1024 个扇区（需要根据实际设备调整）
        uint64_t last_sector = 1023; // 从设备信息获取
        uint8_t read_buf4[512] = {0};

        req.type = VIRTIO_BLK_T_IN;
        req.sector = last_sector;
        req.ioprio = 0;

        ret = virtio_disk_rw_sync(disk, &req, read_buf4, 512, &status);

        if (ret > 0)
        {
                printk("  ✓ Read last sector (%lu) success: %d bytes\n", last_sector, ret);
        }
        else
        {
                printk("  ✗ Read last sector failed: %d (status=%d)\n", ret, status);
        }

        printk("\n[Test 5] Error test: non-512 aligned bytes\n");

        req.type = VIRTIO_BLK_T_IN;
        req.sector = 0;
        req.ioprio = 0;

        ret = virtio_disk_rw_sync(disk, &req, read_buf1, 100, &status); // 100不是512的倍数

        if (ret == -1)
        {
                printk("  ✓ Correctly rejected non-aligned request (100 bytes)\n");
        }
        else
        {
                printk("  ✗ Should have rejected non-aligned request, got %d\n", ret);
        }

        printk("\n[Test 6] Stress test: 10 consecutive reads\n");

        int success_count = 0;
        for (int i = 0; i < 10; i++)
        {
                uint8_t buf[512] = {0};
                req.type = VIRTIO_BLK_T_IN;
                req.sector = i;
                req.ioprio = 0;

                ret = virtio_disk_rw_sync(disk, &req, buf, 512, &status);
                if (ret > 0)
                {
                        success_count++;
                }
        }
        printk("  ✓ %d/10 reads succeeded\n", success_count);

        printk("\n========== VirtIO Disk Test Complete ==========\n");
}