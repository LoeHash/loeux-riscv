#include <fat12.h>
#include <memory.h>
#include <vm.h>
#include <block_device.h>
#include <printk.h>
#include <vfs.h>
#include <lib.h>

struct file_operation fat12_ops = {
    .fs_close = fat12_close,
    .fs_create = fat12_create,
    .fs_free_node = fat12_free_node,
    .fs_lookup = fat12_lookup,
    .fs_mount = fat12_mount,
    .fs_open = fat12_open,
    .fs_read = fat12_read,
    .fs_write = fat12_write};

static int find_in_dir(struct fat12_priv *fs, uint32_t start_sector, uint32_t dir_size,
                       const char *filename, struct fat12_dirent *out);
static int match_dos_name(struct fat12_dirent *e, const char *name);
static uint32_t fat12_cluster_to_sector(struct fat12_priv *fs, uint16_t cluster);
static uint16_t fat12_read_fat_entry(struct fat12_priv *fs, uint16_t cluster);
static void fat12_write_fat_entry(struct fat12_priv *fs, uint16_t cluster, uint16_t value);
static uint16_t fat12_alloc_cluster(struct fat12_priv *fs);
static void fat12_parse_filename(const char *filename, uint8_t *name, uint8_t *ext);
static void fat12_update_dirent_size(struct fat12_priv *fs, struct fat12_node *fnode);
static int fat12_expand_file(struct fat12_priv *fs, struct fat12_node *fnode, uint32_t new_size);
static int fat12_create_dirent(struct fat12_priv *fs, uint32_t *start_sector, uint32_t *dir_size,
                               const char *filename, uint16_t cluster, int mode);
static uint16_t fat12_sector_to_cluster(struct fat12_priv *fs, uint32_t sector);
static void fat12_update_dir_size(struct fat12_priv *fs, struct fat12_node *dir_node);

int fat12_create(void *fs_priv, const char *rel_path, int mode)
{
        if (!fs_priv || !rel_path)
        {
                return -1;
        }

        struct fat12_priv *fs = (struct fat12_priv *)fs_priv;
        char filename[256];
        const char *path = rel_path;
        uint32_t parent_sector;
        uint32_t parent_size;
        int ret;

        // 跳过开头的 '/'
        while (*path == '/')
                path++;

        if (*path == '\0')
        {
                return -1; // 不能创建根目录
        }

        // 提取文件名
        int i = 0;
        while (path[i] && path[i] != '/')
        {
                filename[i] = path[i];
                i++;
        }
        filename[i] = '\0';

        // 如果还有下一级，暂不支持
        if (path[i] == '/')
        {
                return -1; // 暂不支持子目录创建
        }

        // 从根目录开始查找父目录
        parent_sector = fs->root_dir_start;
        parent_size = fs->root_entries * 32;

        // 检查文件是否已存在
        struct fat12_dirent entry;
        ret = find_in_dir(fs, parent_sector, parent_size, filename, &entry);
        if (ret == 0)
        {
                return -1; // 文件已存在
        }

        // 分配一个空闲簇
        uint16_t cluster = fat12_alloc_cluster(fs);
        if (cluster == 0)
        {
                return -1; // 磁盘已满
        }

        // 标记簇为结束
        fat12_write_fat_entry(fs, cluster, 0xFFF);

        // 在目录中创建目录项如果目录满了，会自动扩展
        ret = fat12_create_dirent(fs, &parent_sector, &parent_size, filename, cluster, mode);
        if (ret < 0)
        {
                // 回滚
                fat12_write_fat_entry(fs, cluster, 0);
                return -1;
        }

        return 0;
}

static uint16_t fat12_sector_to_cluster(struct fat12_priv *fs, uint32_t sector)
{
        if (sector < fs->data_start)
        {
                return 0; // 不是数据区
        }
        return (sector - fs->data_start) / fs->sectors_per_cluster + 2;
}

static void fat12_update_dir_size(struct fat12_priv *fs, struct fat12_node *dir_node)
{
        // 遍历目录的所有簇，计算实际大小
        uint32_t total_size = 0;
        uint16_t cluster = dir_node->start_cluster;

        while (1)
        {
                total_size += fs->sectors_per_cluster * 512;
                cluster = fat12_read_fat_entry(fs, cluster);
                if (cluster >= 0xFF8)
                        break;
        }

        dir_node->file_size = total_size;
        fat12_update_dirent_size(fs, dir_node);
}

static int fat12_create_dirent(struct fat12_priv *fs, uint32_t *start_sector, uint32_t *dir_size,
                               const char *filename, uint16_t cluster, int mode)
{
        uint8_t *buf = alloc_page();
        if (!buf)
                return -1;

        int sector_count = *dir_size / 512;
        if (*dir_size % 512)
                sector_count++;

        // 遍历所有扇区
        for (int s = 0; s < sector_count; s++)
        {
                fs->bdev->driver.read(fs->bdev->private_data, *start_sector + s, buf);

                for (int i = 0; i < 16; i++)
                {
                        struct fat12_dirent *e = (struct fat12_dirent *)(buf + i * 32);

                        // 找到空闲项
                        if (e->dir_name[0] == 0x00 || e->dir_name[0] == 0xE5)
                        {
                                // 填充目录项
                                fat12_parse_filename(filename, e->dir_name, e->dir_ext);

                                e->dir_attr = FAT12_ATTR_ARCHIVE;
                                e->dir_nt_reserved = 0;
                                e->dir_create_time_tenth = 0;
                                e->dir_create_time = 0;
                                e->dir_create_date = 0;
                                e->dir_last_access_date = 0;
                                e->dir_first_cluster_high = 0;
                                e->dir_write_time = 0;
                                e->dir_write_date = 0;
                                e->dir_first_cluster_low = cluster;
                                e->dir_file_size = 0;

                                // 写回扇区
                                fs->bdev->driver.write(fs->bdev->private_data, *start_sector + s, buf);
                                free_page(buf);
                                return 0;
                        }
                }
        }

        // 目录已满，需要扩展
        // 检查是否是根目录（根目录不能扩展，因为是固定大小）
        if (*start_sector == fs->root_dir_start)
        {
                free_page(buf);
                return -1; // 根目录已满
        }

        // 扩展目录（分配新簇）
        // 需要找到目录对应的 node（或者通过父目录的起始簇号）
        // 这里简化：假设我们知道父目录的簇号
        // 实际应该通过传入父目录 node 来处理

        // 分配新簇
        uint16_t new_cluster = fat12_alloc_cluster(fs);
        if (new_cluster == 0)
        {
                free_page(buf);
                return -1; // 磁盘已满
        }

        // 清空新簇（所有扇区写0）
        uint32_t new_sector = fat12_cluster_to_sector(fs, new_cluster);
        memset(buf, 0, 512);
        for (uint8_t i = 0; i < fs->sectors_per_cluster; i++)
        {
                fs->bdev->driver.write(fs->bdev->private_data, new_sector + i, buf);
        }

        // 链接到目录链末尾
        // 找到目录的最后一个簇
        // 需要知道目录的起始簇号，这里假设通过查找得到
        // 简化：通过 *start_sector 反推簇号
        uint16_t last_cluster = fat12_sector_to_cluster(fs, *start_sector);
        while (1)
        {
                uint16_t next = fat12_read_fat_entry(fs, last_cluster);
                if (next >= 0xFF8)
                        break;
                last_cluster = next;
        }
        fat12_write_fat_entry(fs, last_cluster, new_cluster);
        fat12_write_fat_entry(fs, new_cluster, 0xFFF);

        // 更新目录大小
        *dir_size += fs->sectors_per_cluster * 512;

        // 更新目录的目录项中的大小字段（需要找到目录对应的目录项）
        // 这里简化，实际需要更新父目录中的目录项
        // 可以通过查找目录名来更新

        // 在新簇的第一个扇区创建目录项
        sector_count = *dir_size / 512;
        fs->bdev->driver.read(fs->bdev->private_data, new_sector, buf);

        for (int i = 0; i < 16; i++)
        {
                struct fat12_dirent *e = (struct fat12_dirent *)(buf + i * 32);
                if (e->dir_name[0] == 0x00 || e->dir_name[0] == 0xE5)
                {
                        fat12_parse_filename(filename, e->dir_name, e->dir_ext);
                        e->dir_attr = FAT12_ATTR_ARCHIVE;
                        e->dir_nt_reserved = 0;
                        e->dir_create_time_tenth = 0;
                        e->dir_create_time = 0;
                        e->dir_create_date = 0;
                        e->dir_last_access_date = 0;
                        e->dir_first_cluster_high = 0;
                        e->dir_write_time = 0;
                        e->dir_write_date = 0;
                        e->dir_first_cluster_low = cluster;
                        e->dir_file_size = 0;

                        fs->bdev->driver.write(fs->bdev->private_data, new_sector, buf);
                        free_page(buf);
                        return 0;
                }
        }

        free_page(buf);
        return -1;
}

int fat12_close(struct file *file)
{
        if (!file || !file->private)
        {
                return -1;
        }

        return 0;
}

void fat12_free_node(void *out_node)
{
        free_page(out_node);
}

int fat12_write(struct file *file, const void *buf, uint64_t count, uint64_t *out_len)
{
        if (!file || !buf || !out_len)
        {
                return -1;
        }

        struct fat12_node *fnode = (struct fat12_node *)file->private;
        struct fat12_priv *fs = fnode->fs_priv;
        uint64_t bytes_written = 0;
        uint32_t offset = file->pos;
        uint8_t sector_buf[512];

        // 只读文件不能写
        if (fnode->attr & FAT12_ATTR_READ_ONLY)
        {
                return -1;
        }

        // 目录不能写
        if (fnode->attr & FAT12_ATTR_DIRECTORY)
        {
                return -1;
        }

        // 如果写入位置超出文件大小，需要扩展
        uint32_t new_size = offset + count;
        if (new_size > fnode->file_size)
        {
                int ret = fat12_expand_file(fs, fnode, new_size);
                if (ret < 0)
                {
                        *out_len = 0;
                        return -1; // 磁盘已满或其他错误
                }
        }

        const uint8_t *src = (const uint8_t *)buf;
        uint32_t current_offset = offset;

        // 写入数据
        for (uint32_t i = 0; i < count; i++)
        {
                // 计算当前簇
                uint16_t cluster = fnode->start_cluster;
                uint32_t cluster_offset = current_offset / (fs->sectors_per_cluster * 512);

                // 跳转到目标簇
                for (uint32_t j = 0; j < cluster_offset; j++)
                {
                        cluster = fat12_read_fat_entry(fs, cluster);
                        if (cluster >= 0xFF8)
                        {
                                *out_len = i;
                                file->pos += i;
                                return 0;
                        }
                }

                // 计算扇区和偏移
                uint32_t cluster_byte_offset = current_offset % (fs->sectors_per_cluster * 512);
                uint32_t sector = fat12_cluster_to_sector(fs, cluster) + cluster_byte_offset / 512;
                uint32_t byte_off = cluster_byte_offset % 512;

                // 读扇区到缓存
                if (fs->bdev->driver.read(fs->bdev->private_data, sector, sector_buf) < 0)
                {
                        *out_len = i;
                        file->pos += i;
                        return -1;
                }

                // 修改字节
                sector_buf[byte_off] = src[i];

                // 写回扇区
                if (fs->bdev->driver.write(fs->bdev->private_data, sector, sector_buf) < 0)
                {
                        *out_len = i;
                        file->pos += i;
                        return -1;
                }

                bytes_written++;
                current_offset++;
        }

        // file->pos += bytes_written;
        *out_len = bytes_written;
        return 0;
}

static int fat12_expand_file(struct fat12_priv *fs, struct fat12_node *fnode, uint32_t new_size)
{
        uint32_t old_size = fnode->file_size;
        uint32_t old_clusters = (old_size + fs->sectors_per_cluster * 512 - 1) / (fs->sectors_per_cluster * 512);
        uint32_t new_clusters = (new_size + fs->sectors_per_cluster * 512 - 1) / (fs->sectors_per_cluster * 512);

        // 如果簇数没变，只需要更新大小
        if (new_clusters <= old_clusters)
        {
                fnode->file_size = new_size;
                fat12_update_dirent_size(fs, fnode);
                return 0;
        }

        // 需要分配新簇
        uint32_t need = new_clusters - old_clusters;

        // 找到文件最后一个簇
        uint16_t last_cluster = fnode->start_cluster;
        if (old_clusters == 0)
        {
                // 空文件，分配第一个簇
                uint16_t first = fat12_alloc_cluster(fs);
                if (first == 0)
                        return -1;
                fnode->start_cluster = first;
                last_cluster = first;
                old_clusters = 1;
                need--;
        }
        else
        {
                while (1)
                {
                        uint16_t next = fat12_read_fat_entry(fs, last_cluster);
                        if (next >= 0xFF8)
                                break;
                        last_cluster = next;
                }
        }

        // 分配新簇
        for (uint32_t i = 0; i < need; i++)
        {
                uint16_t new_cluster = fat12_alloc_cluster(fs);
                if (new_cluster == 0)
                {
                        // 磁盘已满，回滚
                        fat12_write_fat_entry(fs, last_cluster, 0xFFF);
                        return -1;
                }
                fat12_write_fat_entry(fs, last_cluster, new_cluster);
                last_cluster = new_cluster;
        }
        fat12_write_fat_entry(fs, last_cluster, 0xFFF);

        // 更新文件大小
        fnode->file_size = new_size;
        fat12_update_dirent_size(fs, fnode);

        return 0;
}

int fat12_read(struct file *file, void *buf, uint64_t count, uint64_t *out_len)
{
        if (!file || !buf || !out_len)
        {
                return -1;
        }

        struct fat12_node *fnode = (struct fat12_node *)file->private;
        struct fat12_priv *fs = fnode->fs_priv;
        uint8_t sector_buf[512];
        uint64_t bytes_read = 0;
        uint32_t offset = file->pos;

        // 如果已经到文件末尾
        if (offset >= fnode->file_size)
        {
                *out_len = 0;
                return 0;
        }

        // 限制读取大小
        if (offset + count > fnode->file_size)
        {
                count = fnode->file_size - offset;
        }

        // 计算起始簇号和簇内偏移
        uint32_t cluster_offset = offset / (fs->sectors_per_cluster * 512);
        uint32_t cluster = fnode->start_cluster;

        // 跳转到起始簇
        for (uint32_t i = 0; i < cluster_offset; i++)
        {
                cluster = fat12_read_fat_entry(fs, cluster);
                if (cluster >= 0xFF8)
                {
                        *out_len = bytes_read;
                        return 0;
                }
        }

        // 计算簇内偏移
        uint32_t sector_offset = (offset % (fs->sectors_per_cluster * 512)) / 512;
        uint32_t byte_offset = (offset % (fs->sectors_per_cluster * 512)) % 512;

        // 读取数据
        while (bytes_read < count)
        {
                uint32_t sector = fat12_cluster_to_sector(fs, cluster) + sector_offset;

                // 读扇区
                if (fs->bdev->driver.read(fs->bdev->private_data, sector, sector_buf) < 0)
                {
                        *out_len = bytes_read;
                        return -1;
                }

                // 复制数据
                uint32_t copy_len = 512 - byte_offset;
                if (bytes_read + copy_len > count)
                {
                        copy_len = count - bytes_read;
                }
                if (bytes_read + copy_len > fnode->file_size - offset)
                {
                        copy_len = fnode->file_size - offset - bytes_read;
                }

                memcpy(buf + bytes_read, sector_buf + byte_offset, copy_len);
                bytes_read += copy_len;
                byte_offset = 0;
                sector_offset++;

                // 如果当前簇读完，跳到下一个簇
                if (sector_offset >= fs->sectors_per_cluster)
                {
                        sector_offset = 0;
                        cluster = fat12_read_fat_entry(fs, cluster);
                        if (cluster >= 0xFF8)
                        {
                                break;
                        }
                }
        }

        // 更新文件位置
        // 所有的文件系统都不应直接修改file的任何属性，原因如下:
        // 1. 语义不明
        // 2. 与vfs层逻辑混乱
        // 3. seeking...
        // file->pos += bytes_read;
        *out_len = bytes_read;
        return 0;
}

int fat12_open(void *node, struct file *file, int flags)
{
        if (!node || !file)
        {
                return -1;
        }

        struct fat12_node *fnode = (struct fat12_node *)node;

        // 检查是否是目录
        if (fnode->attr & FAT12_ATTR_DIRECTORY)
        {
                // 目录只能以读方式打开
                if (flags & FS_O_WRITE)
                {
                        return -1;
                }
        }

        // 检查只读属性
        if (fnode->attr & FAT12_ATTR_READ_ONLY)
        {
                if (flags & FS_O_WRITE)
                {
                        return -1;
                }
        }

        // 如果是写或读写模式，但文件是只读属性，拒绝
        if (flags & FS_O_WRITE)
        {
                if (fnode->attr & FAT12_ATTR_READ_ONLY)
                {
                        return -1;
                }
        }

        // 创建标志 (如果文件不存在，由 VFS 层处理)
        // fat12_open 只负责打开已存在的文件

        // 截断标志
        if (flags & FS_O_TRUNC)
        {
                // 需要实现 fat12_truncate 函数
                // 暂时返回错误
                return -1;
        }

        // 追加标志
        if (flags & FS_O_APPEND)
        {
                file->pos = fnode->file_size;
        }
        else
        {
                file->pos = 0;
        }

        return 0;
}

int fat12_lookup(void *fs_priv, const char *rel_path, void **out_node)
{

        if (!fs_priv || !rel_path || !out_node)
        {
                return -1;
        }

        struct fat12_priv *fs_p = (struct fat12_priv *)fs_priv;
        struct fat12_dirent entry;
        struct fat12_node *node;
        char filename[256];
        int ret;

        // 跳过开头的 '/'
        const char *path = rel_path;
        while (*path == '/')
                path++;

        // 如果是根目录 "/"
        if (*path == '\0')
        {
                node = (struct fat12_node *)alloc_page();
                if (!node)
                        return -1;
                node->is_root = 1;
                node->fs_priv = fs_p;
                *out_node = node;
                return 0;
        }

        // 从根目录开始查找
        uint32_t current_sector = fs_p->root_dir_start;
        uint32_t current_size = fs_p->root_entries * 32;

        while (*path)
        {
                // 提取文件名
                int i = 0;
                while (path[i] && path[i] != '/')
                {
                        filename[i] = path[i];
                        i++;
                }
                filename[i] = '\0';

                // 在当前目录查找
                ret = find_in_dir(fs_priv, current_sector, current_size, filename, &entry);
                if (ret < 0)
                {
                        return -1;
                }

                // 如果还有下一级，必须是目录
                if (path[i] == '/')
                {
                        if (!(entry.dir_attr & 0x10))
                        {
                                return -1;
                        }
                        uint16_t cluster = entry.dir_first_cluster_low;
                        current_sector = fs_p->data_start + (cluster - 2) * fs_p->sectors_per_cluster;
                        current_size = entry.dir_file_size;
                        path += i + 1;
                        continue;
                }

                // 找到了
                node = (struct fat12_node *)alloc_page();
                if (!node)
                {
                        return -1;
                }

                node->is_root = 0;
                node->fs_priv = fs_priv;
                node->start_cluster = entry.dir_first_cluster_low;
                node->file_size = entry.dir_file_size;
                node->attr = entry.dir_attr;
                *out_node = node;
                return node->file_size;
        }

        return -1;
}

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

static int find_in_dir(struct fat12_priv *fs, uint32_t start_sector, uint32_t dir_size,
                       const char *filename, struct fat12_dirent *out)
{
        uint8_t *buf = alloc_page();
        if (!buf)
                return -1;

        int sector_count = dir_size / 512;
        if (dir_size % 512)
                sector_count++;

        for (int s = 0; s < sector_count; s++)
        {
                // 读扇区
                fs->bdev->driver.read(fs->bdev->private_data, start_sector + s, buf);

                for (int i = 0; i < 16; i++)
                {
                        struct fat12_dirent *e = (struct fat12_dirent *)(buf + i * 32);

                        if (e->dir_name[0] == 0x00)
                        {
                                free_page(buf);
                                return -1;
                        }
                        if (e->dir_name[0] == 0xE5)
                                continue;
                        if (e->dir_attr == 0x0F)
                                continue; // 长文件名
                        if (e->dir_attr == 0x08)
                                continue; // 卷标

                        if (match_dos_name(e, filename))
                        {
                                *out = *e;
                                free_page(buf);
                                return 0;
                        }
                }
        }
        free_page(buf);
        return -1;
}

// FAT表项读取
static uint16_t fat12_read_fat_entry(struct fat12_priv *fs, uint16_t cluster)
{
        uint32_t offset = cluster * 3 / 2;
        uint8_t *fat = fs->fat_table + offset;

        if (cluster & 1)
        {
                return ((fat[1] & 0x0F) << 8) | fat[0];
        }
        else
        {
                return (fat[0] & 0x0F) | (fat[1] << 4);
        }
}

// 簇号转扇区
static uint32_t fat12_cluster_to_sector(struct fat12_priv *fs, uint16_t cluster)
{
        return fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
}

static uint16_t fat12_alloc_cluster(struct fat12_priv *fs)
{
        // 从簇2开始查找（簇0和簇1保留）
        for (uint16_t i = 2; i < fs->cluster_count; i++)
        {
                if (fat12_read_fat_entry(fs, i) == 0)
                {
                        // 标记为已使用（暂时标记为EOF，后续由调用者修改）
                        fat12_write_fat_entry(fs, i, 0xFFF);
                        return i;
                }
        }
        return 0; // 没有空闲簇
}

static void fat12_write_fat_entry(struct fat12_priv *fs, uint16_t cluster, uint16_t value)
{
        // 每个表项12位 = 1.5字节
        uint32_t offset = cluster * 3 / 2;
        uint8_t *fat = fs->fat_table + offset;

        // 根据簇号奇偶性写入
        if (cluster & 1)
        {
                // 奇数簇: 低4位在fat[0]的高4位，高8位在fat[1]
                fat[0] = (fat[0] & 0x0F) | ((value & 0x0F) << 4);
                fat[1] = value >> 4;
        }
        else
        {
                // 偶数簇: 低8位在fat[0]，高4位在fat[1]的低4位
                fat[0] = value & 0xFF;
                fat[1] = (fat[1] & 0xF0) | ((value >> 8) & 0x0F);
        }

        // 写回磁盘（只写修改的扇区）
        uint32_t sector = fs->fat_start + offset / 512;
        fs->bdev->driver.write(fs->bdev->private_data, sector, fat - (offset % 512));
}

static void fat12_update_dirent_size(struct fat12_priv *fs, struct fat12_node *fnode)
{
        uint8_t *buf = alloc_page();
        if (!buf)
                return;

        // 从根目录开始查找
        uint32_t sector = fs->root_dir_start;
        uint32_t sector_count = fs->root_dir_sectors;

        for (uint32_t s = 0; s < sector_count; s++)
        {
                fs->bdev->driver.read(fs->bdev->private_data, sector + s, buf);

                for (int i = 0; i < 16; i++)
                {
                        struct fat12_dirent *e = (struct fat12_dirent *)(buf + i * 32);

                        if (e->dir_name[0] == 0x00)
                        {
                                free_page(buf);
                                return;
                        }
                        if (e->dir_name[0] == 0xE5)
                                continue;
                        if (e->dir_attr == 0x0F)
                                continue;
                        if (e->dir_attr == 0x08)
                                continue;

                        // 比较文件名（8字节文件名 + 3字节扩展名）
                        if (memcmp(e->dir_name, fnode->name, 8) == 0 &&
                            memcmp(e->dir_ext, fnode->name + 8, 3) == 0)
                        {
                                // 更新文件大小
                                e->dir_file_size = fnode->file_size;
                                fs->bdev->driver.write(fs->bdev->private_data, sector + s, buf);
                                free_page(buf);
                                return;
                        }
                }
        }
        free_page(buf);
}

static void fat12_parse_filename(const char *filename, uint8_t *name, uint8_t *ext)
{
        // 初始化为空格
        for (int i = 0; i < 8; i++)
                name[i] = ' ';
        for (int i = 0; i < 3; i++)
                ext[i] = ' ';

        // 查找点号分隔符
        const char *dot = filename;
        while (*dot && *dot != '.')
                dot++;

        // 复制文件名（最多8字节）
        int name_len = dot - filename;
        if (name_len > 8)
                name_len = 8;
        for (int i = 0; i < name_len; i++)
        {
                name[i] = filename[i];
                // 转大写
                if (name[i] >= 'a' && name[i] <= 'z')
                {
                        name[i] -= 'a' - 'A';
                }
        }

        // 复制扩展名（最多3字节）
        if (*dot == '.')
        {
                int ext_len = 0;
                const char *p = dot + 1;
                while (*p && ext_len < 3)
                {
                        ext[ext_len++] = *p;
                        // 转大写
                        if (ext[ext_len - 1] >= 'a' && ext[ext_len - 1] <= 'z')
                        {
                                ext[ext_len - 1] -= 'a' - 'A';
                        }
                        p++;
                }
        }
}

static int match_dos_name(struct fat12_dirent *e, const char *name)
{
        char dos_name[13];
        int j = 0;

        // 构建8.3格式文件名
        for (int i = 0; i < 8; i++)
        {
                if (e->dir_name[i] != ' ')
                {
                        dos_name[j++] = e->dir_name[i];
                }
        }

        int has_ext = 0;
        for (int i = 0; i < 3; i++)
        {
                if (e->dir_ext[i] != ' ')
                {
                        if (!has_ext)
                        {
                                dos_name[j++] = '.';
                                has_ext = 1;
                        }
                        dos_name[j++] = e->dir_ext[i];
                }
        }
        dos_name[j] = '\0';

        return strcasecmp(dos_name, name) == 0;
}