#include <virtio.h>
#include <test.h>

void test_write_verify()
{
        struct virtio_blk_disk *disk = &usable_disks[0];
        struct virtio_blk_req req;
        uint8_t status;
        uint8_t write_buf[512], read_buf[512] = {0};

        // // 构造数据
        // for (int i = 0; i < 512; i++)
        //         write_buf[i] = i & 0xFF;
        // strcpy((char *)write_buf, "Hello VirtIO!");
        memset(write_buf, 0x14, 512);

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

void dump_sector_n(struct virtio_blk_disk *disk, int n)
{
        struct virtio_blk_req req;
        uint8_t status;
        uint8_t buf[512] = {0};
        int ret;

        req.type = VIRTIO_BLK_T_IN;
        req.sector = n;
        req.ioprio = 0;

        ret = virtio_disk_rw_sync(disk, &req, buf, 512, &status);

        if (ret < 0)
        {
                printk("Read sector 0 failed: status=%d\n", status);
                return;
        }

        printk("=== Sector %d Dump (512 bytes) ===\n", n);
        for (int i = 0; i < 512; i++)
        {
                if (i % 16 == 0)
                {
                        printk("\n%04x: ", i);
                }
                printk("%02x ", buf[i]);
        }
        printk("\n=== End of Dump ===\n");
}