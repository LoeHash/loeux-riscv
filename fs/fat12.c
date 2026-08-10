#include <fat12.h>
#include <memory.h>
#include <block_device.h>
#include <vfs.h>

struct file_operation fat12_ops = {0};

void *fat12_mount(struct block_device *bdev)
{
        struct fat12_priv *f12_priv = alloc_page();
        if (!f12_priv)
                return NULL;

        uint8_t *buffer = alloc_page();
        if (!buffer)
        {
                free_page(f12_priv);
                return NULL;
        }

        // 读引导扇区
        bdev->driver.read(bdev->private_data, 0, buffer);

        // 校验签名
        if (buffer[510] != 0x55 || buffer[511] != 0xAA)
        {
                free_page(buffer);
                free_page(f12_priv);
                return NULL;
        }

        // 从 buffer 填 BPB 字段 (小端)
        f12_priv->bytes_per_sector = buffer[11] | (buffer[12] << 8);
        f12_priv->sectors_per_cluster = buffer[13];
        f12_priv->reserved_sectors = buffer[14] | (buffer[15] << 8);
        f12_priv->fat_count = buffer[16];
        f12_priv->root_entries = buffer[17] | (buffer[18] << 8);
        f12_priv->total_sectors = buffer[19] | (buffer[20] << 8);
        f12_priv->sectors_per_fat = buffer[22] | (buffer[23] << 8);

        // 校验关键字段
        if (f12_priv->bytes_per_sector != 512)
        {
                free_page(buffer);
                free_page(f12_priv);
                return NULL;
        }

        // 计算区域
        f12_priv->fat_start = f12_priv->reserved_sectors;
        f12_priv->root_dir_start = f12_priv->fat_start + f12_priv->fat_count * f12_priv->sectors_per_fat;
        f12_priv->root_dir_sectors = (f12_priv->root_entries * 32 + f12_priv->bytes_per_sector - 1) / f12_priv->bytes_per_sector;
        f12_priv->data_start = f12_priv->root_dir_start + f12_priv->root_dir_sectors;
        f12_priv->cluster_count = (f12_priv->total_sectors - f12_priv->data_start) / f12_priv->sectors_per_cluster;

        // 读 FAT 表到内存
        uint32_t fat_table_size = f12_priv->sectors_per_fat * f12_priv->bytes_per_sector;
        f12_priv->fat_table = alloc_page();
        if (!f12_priv->fat_table)
        {
                free_page(buffer);
                free_page(f12_priv);
                return NULL;
        }

        // 读取所有 FAT 扇区
        for (uint8_t i = 0; i < f12_priv->sectors_per_fat; i++)
        {
                bdev->driver.read(bdev->private_data, f12_priv->fat_start + i, f12_priv->fat_table + i * 512);
        }

        // 存 bdev
        f12_priv->bdev = bdev;

        free_page(buffer);
        return (void *)f12_priv;
}