#include <fat12.h>
#include <memory.h>
#include <vm.h>
#include <block_device.h>
#include <printk.h>
#include <vfs.h>
#include <lib.h>
#include <timer.h>

struct file_operation fat12_ops = {
    .fs_close = fat12_close,
    .fs_create = fat12_create,
    .fs_free_node = fat12_free_node,
    .fs_lookup = fat12_lookup,
    .fs_mount = fat12_mount,
    .fs_open = fat12_open,
    .fs_read = fat12_read,
    .fs_write = fat12_write,
    .fs_is_dir = fat12_is_dir,
    .fs_getattr = fat12_getattr,
    .fs_readdir = fat12_readdir};

static int find_in_dir(struct fat12_priv *fs, uint32_t start_sector, uint32_t dir_size,
                       const char *filename, struct fat12_dirent *out,
                       uint32_t *out_sector, uint16_t *out_off);
static int match_dos_name(struct fat12_dirent *e, const char *name);
static uint32_t fat12_cluster_to_sector(struct fat12_priv *fs, uint16_t cluster);
static uint16_t fat12_read_fat_entry(struct fat12_priv *fs, uint16_t cluster);
static void fat12_write_fat_entry(struct fat12_priv *fs, uint16_t cluster, uint16_t value);
static uint16_t fat12_alloc_cluster(struct fat12_priv *fs);
static void fat12_parse_filename(const char *filename, uint8_t *name, uint8_t *ext);
static void fat12_update_dirent_size(struct fat12_priv *fs, struct fat12_node *fnode);
static int fat12_expand_file(struct fat12_priv *fs, struct fat12_node *fnode, uint32_t new_size);
static int fat12_create_dirent(struct fat12_priv *fs, uint32_t *start_sector, uint32_t *dir_size,
                               const char *filename, uint16_t cluster, int fat12_attr,
                               uint16_t parent_start_cluster);
static void fat12_set_dirent_timestamps(struct fat12_dirent *e);
static uint8_t fat12_attr_from_generic(file_attr_t attr);
static void fat12_set_dirent_case_flags(struct fat12_dirent *e, const char *filename);
static void fat12_init_new_dir(struct fat12_priv *fs, uint16_t cluster, uint16_t parent_cluster);
static uint16_t fat12_sector_to_cluster(struct fat12_priv *fs, uint32_t sector);
static void fat12_update_dir_size(struct fat12_priv *fs, struct fat12_node *dir_node);
static uint32_t fat12_dir_logical_size(struct fat12_priv *fs, struct fat12_node *dir);

/// @brief 创建文件或目录
/// @param attr 通用文件属性，由 VFS 层传入，各文件系统自行转换为内部格式
int fat12_create(void *fs_priv, const char *rel_path, file_attr_t attr)
{
        if (!fs_priv || !rel_path)
        {
                return -1;
        }

        struct fat12_priv *fs = (struct fat12_priv *)fs_priv;
        uint8_t fat12_attr = fat12_attr_from_generic(attr);

        char filename[256];
        const char *path = rel_path;
        uint32_t parent_sector;
        uint32_t parent_size;
        uint16_t parent_start_cluster = 0; // 0 = 根目录
        int ret;

        // 跳过开头的 '/'
        while (*path == '/')
                path++;

        if (*path == '\0')
        {
                return -1; // 不能创建根目录
        }

        // 遍历路径组件，找到实际的父目录
        // 例如 "tmp/foo.txt" -> 先找到 tmp 目录，再在其中创建 foo.txt
        parent_sector = fs->root_dir_start;
        parent_size = fs->root_entries * 32;
        parent_start_cluster = 0; // 根目录

        while (1)
        {
                // 提取当前组件
                int i = 0;
                while (path[i] && path[i] != '/')
                {
                        filename[i] = path[i];
                        i++;
                }
                filename[i] = '\0';

                if (path[i] == '\0')
                {
                        // 最后一个组件 -> 这是要创建的文件/目录名
                        break;
                }

                // 还有下一级 -> 当前组件必须是目录
                struct fat12_dirent entry;
                ret = find_in_dir(fs, parent_sector, parent_size, filename, &entry, NULL, NULL);
                if (ret < 0)
                {
                        // printk("fat12_create: parent component '%s' not found\n", filename);
                        return -1;
                }
                if (!(entry.dir_attr & FAT12_ATTR_DIRECTORY))
                {
                        return -1; // 不是目录
                }

                // 进入该子目录
                uint16_t cluster = entry.dir_first_cluster_low;
                parent_sector = fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
                parent_size = entry.dir_file_size;
                if (parent_size == 0)
                {
                        parent_size = fs->sectors_per_cluster * 512;
                }
                parent_start_cluster = cluster;
                path += i + 1;
        }

        // 检查文件是否已存在
        struct fat12_dirent entry;
        ret = find_in_dir(fs, parent_sector, parent_size, filename, &entry, NULL, NULL);
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

        // printk("fat12_create: name='%s' cluster=%d attr=0x%02x parent_cluster=%d\n",
        //        filename, cluster, fat12_attr, parent_start_cluster);

        // 标记簇为结束
        fat12_write_fat_entry(fs, cluster, 0xFFF);

        // 在父目录中创建目录项
        ret = fat12_create_dirent(fs, &parent_sector, &parent_size, filename, cluster,
                                  fat12_attr, parent_start_cluster);
        if (ret < 0)
        {
                // 回滚
                fat12_write_fat_entry(fs, cluster, 0);
                return -1;
        }

        // 如果是目录，初始化 . 和 .. 条目
        if (attr.is_dir)
        {
                fat12_init_new_dir(fs, cluster, parent_start_cluster);

                // 更新父目录中该目录项的 file_size 为一个簇的大小
                struct fat12_node dir_fnode;
                memset(&dir_fnode, 0, sizeof(dir_fnode));
                fat12_parse_filename(filename, dir_fnode.name, dir_fnode.name + 8);
                dir_fnode.start_cluster = cluster;
                dir_fnode.file_size = fs->sectors_per_cluster * 512;
                fat12_update_dirent_size(fs, &dir_fnode);
                // printk("fat12_create: dir size updated to %d\n", dir_fnode.file_size);
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

/// @brief 将通用文件属性转换为 FAT12 属性字节
static uint8_t fat12_attr_from_generic(file_attr_t attr)
{
        uint8_t fat_attr = 0;
        if (attr.is_dir)
                fat_attr |= FAT12_ATTR_DIRECTORY;
        if (!attr.writable)
                fat_attr |= FAT12_ATTR_READ_ONLY;
        if (!attr.is_dir)
                fat_attr |= FAT12_ATTR_ARCHIVE;
        return fat_attr;
}

/// @brief 检查文件名是否包含小写字母
static int fat12_has_lower(const char *s)
{
        while (*s)
        {
                if (*s >= 'a' && *s <= 'z')
                        return 1;
                s++;
        }
        return 0;
}

/// @brief 设置目录项的 NT 大小写标志
/// FAT12 文件名在磁盘上存为大写，但通过 dir_nt_reserved 中的标志位
/// 可告知读取方原始文件名含小写，从而正确显示。
static void fat12_set_dirent_case_flags(struct fat12_dirent *e, const char *filename)
{
        uint8_t nt_flags = 0;
        // 检查文件名部分（点号前）是否有小写
        const char *dot = filename;
        while (*dot && *dot != '.')
                dot++;
        int name_len = dot - filename;
        int has_lower_name = 0;
        for (int i = 0; i < name_len; i++)
        {
                if (filename[i] >= 'a' && filename[i] <= 'z')
                {
                        has_lower_name = 1;
                        break;
                }
        }
        if (has_lower_name)
                nt_flags |= 0x08; // BASE (8.3) name is in lowercase

        // 检查扩展名部分是否有小写
        if (*dot == '.')
        {
                const char *ext = dot + 1;
                while (*ext)
                {
                        if (*ext >= 'a' && *ext <= 'z')
                        {
                                nt_flags |= 0x10; // extension is in lowercase
                                break;
                        }
                        ext++;
                }
        }
        e->dir_nt_reserved = nt_flags;
}

/// @brief 用当前系统时间设置目录项的时间戳字段
static void fat12_set_dirent_timestamps(struct fat12_dirent *e)
{
        uint64_t ticks = get_sys_timer_tick();
        // 假设 100 Hz 时钟频率，转换为 Unix 秒
        uint32_t unix_time = (uint32_t)(ticks / 100) + 1700000000U;

        uint32_t sec = unix_time % 60;
        unix_time /= 60;
        uint32_t min = unix_time % 60;
        unix_time /= 60;
        uint32_t hour = unix_time % 24;
        uint32_t days = unix_time / 24;

        // 计算年月日（从 1970-01-01 开始）
        uint16_t year = 1970;
        uint8_t month = 1;
        uint8_t day;
        static const uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        while (1)
        {
                uint16_t ydays = 365;
                if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
                        ydays = 366;
                if (days >= ydays)
                {
                        days -= ydays;
                        year++;
                }
                else
                        break;
        }

        for (int m = 0; m < 12; m++)
        {
                uint8_t md = days_in_month[m];
                if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
                        md = 29;
                if (days >= md)
                {
                        days -= md;
                        month++;
                }
                else
                        break;
        }
        day = days + 1;

        // FAT 时间格式：年从 1980 开始
        if (year < 1980)
        {
                year = 1980;
                month = 1;
                day = 1;
        }

        uint16_t fat_time = (hour << 11) | (min << 5) | (sec / 2);
        uint16_t fat_date = ((year - 1980) << 9) | (month << 5) | day;

        e->dir_create_time_tenth = 0;
        e->dir_create_time = fat_time;
        e->dir_create_date = fat_date;
        e->dir_write_time = fat_time;
        e->dir_write_date = fat_date;
        e->dir_last_access_date = fat_date;
}

/// @brief 初始化新目录的 . 和 .. 条目
/// 即将过时，目前仅用于测试！
static void fat12_init_new_dir(struct fat12_priv *fs, uint16_t cluster, uint16_t parent_cluster)
{
        uint8_t *buf = alloc_page();
        if (!buf)
                return;

        uint32_t first_sector = fat12_cluster_to_sector(fs, cluster);

        // 清零整个簇的所有扇区（防止残留垃圾数据被当作目录项）
        memset(buf, 0, 512);
        for (uint8_t i = 0; i < fs->sectors_per_cluster; i++)
        {
                fs->bdev->driver.write(fs->bdev->private_data, first_sector + i, buf);
        }

        // "." 条目 -> 指向自身簇
        struct fat12_dirent *dot = (struct fat12_dirent *)buf;
        dot->dir_name[0] = '.';
        for (int i = 1; i < 8; i++)
                dot->dir_name[i] = ' ';
        for (int i = 0; i < 3; i++)
                dot->dir_ext[i] = ' ';
        dot->dir_attr = FAT12_ATTR_DIRECTORY;
        dot->dir_first_cluster_high = 0;
        dot->dir_first_cluster_low = cluster;
        dot->dir_file_size = 0;

        // ".." 条目 -> 指向父目录簇（根目录为 0）
        struct fat12_dirent *dotdot = (struct fat12_dirent *)(buf + 32);
        dotdot->dir_name[0] = '.';
        dotdot->dir_name[1] = '.';
        for (int i = 2; i < 8; i++)
                dotdot->dir_name[i] = ' ';
        for (int i = 0; i < 3; i++)
                dotdot->dir_ext[i] = ' ';
        dotdot->dir_attr = FAT12_ATTR_DIRECTORY;
        dotdot->dir_first_cluster_high = 0;
        dotdot->dir_first_cluster_low = parent_cluster;
        dotdot->dir_file_size = 0;

        // 写回第一扇区（包含 . 和 ..）
        fs->bdev->driver.write(fs->bdev->private_data, first_sector, buf);
        free_page(buf);
}

static int fat12_create_dirent(struct fat12_priv *fs, uint32_t *start_sector, uint32_t *dir_size,
                               const char *filename, uint16_t cluster, int fat12_attr,
                               uint16_t parent_start_cluster)
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

                                e->dir_attr = fat12_attr;
                                fat12_set_dirent_case_flags(e, filename);
                                fat12_set_dirent_timestamps(e);
                                e->dir_first_cluster_high = 0;
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
        // 根目录不能扩展（固定大小）
        if (parent_start_cluster == 0)
        {
                free_page(buf);
                return -1; // 根目录已满
        }

        // 子目录扩展：分配新簇并链接到簇链末尾
        uint16_t new_cluster = fat12_alloc_cluster(fs);
        if (new_cluster == 0)
        {
                free_page(buf);
                return -1; // 磁盘已满
        }

        // 清空新簇
        uint32_t new_sector = fat12_cluster_to_sector(fs, new_cluster);
        memset(buf, 0, 512);
        for (uint8_t i = 0; i < fs->sectors_per_cluster; i++)
        {
                fs->bdev->driver.write(fs->bdev->private_data, new_sector + i, buf);
        }

        // 沿簇链找到最后一个簇
        uint16_t last_cluster = parent_start_cluster;
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

        // 在新簇中创建目录项
        fs->bdev->driver.read(fs->bdev->private_data, new_sector, buf);

        for (int i = 0; i < 16; i++)
        {
                struct fat12_dirent *e = (struct fat12_dirent *)(buf + i * 32);
                if (e->dir_name[0] == 0x00 || e->dir_name[0] == 0xE5)
                {
                        fat12_parse_filename(filename, e->dir_name, e->dir_ext);
                        e->dir_attr = fat12_attr;
                        fat12_set_dirent_case_flags(e, filename);
                        fat12_set_dirent_timestamps(e);
                        e->dir_first_cluster_high = 0;
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

int fat12_is_dir(void *node)
{
        struct fat12_node *fnode = (struct fat12_node *)node;
        if (!fnode)
                return 0;
        return fnode->is_root || (fnode->attr & FAT12_ATTR_DIRECTORY);
}

static int fat12_is_leap(int year)
{
        return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* FAT 日期/时间是本地时间且无时区；按 UTC 解释成 Unix 秒，便于 st_*time。 */
static int64_t fat_datetime_to_unix(uint16_t date, uint16_t time)
{
        int year;
        int month;
        int day;
        int hour;
        int min;
        int sec;
        int64_t days;
        int y;
        int m;
        static const int mdays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        if (date == 0)
                return 0;

        year = ((date >> 9) & 0x7F) + 1980;
        month = (date >> 5) & 0x0F;
        day = date & 0x1F;
        hour = (time >> 11) & 0x1F;
        min = (time >> 5) & 0x3F;
        sec = (time & 0x1F) * 2;

        if (month < 1 || month > 12 || day < 1 || day > 31)
                return 0;

        days = 0;
        for (y = 1970; y < year; y++)
                days += fat12_is_leap(y) ? 366 : 365;
        for (m = 1; m < month; m++)
        {
                days += mdays[m];
                if (m == 2 && fat12_is_leap(year))
                        days++;
        }
        days += day - 1;

        return days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
}

static uint64_t fat12_alloc_blocks_512(struct fat12_node *n)
{
        struct fat12_priv *fs = n->fs_priv;
        uint16_t c;
        uint32_t guard;
        uint64_t clusters = 0;

        if (!fs)
                return 0;

        if (n->is_root)
                return fs->root_dir_sectors;

        if (n->start_cluster < 2)
                return 0;

        c = n->start_cluster;
        guard = fs->cluster_count + 2;
        while (c >= 2 && c < FAT12_EOF && guard--)
        {
                clusters++;
                c = fat12_read_fat_entry(fs, c);
        }

        return clusters * fs->sectors_per_cluster;
}

int fat12_getattr(void *node, struct vfs_kstat *out)
{
        struct fat12_node *n = (struct fat12_node *)node;
        struct fat12_priv *fs;
        int dir;
        uint32_t cluster_bytes;

        if (!n || !out || !n->fs_priv)
                return -1;

        fs = n->fs_priv;
        memset(out, 0, sizeof(*out));

        dir = fat12_is_dir(n);
        cluster_bytes = (uint32_t)fs->sectors_per_cluster * fs->bytes_per_sector;
        if (cluster_bytes == 0)
                cluster_bytes = FAT12_SECTOR_SIZE;

        if (n->is_root)
        {
                out->ino = 1;
                out->size = (uint64_t)fs->root_dir_sectors * fs->bytes_per_sector;
                out->nlink = 2;
                out->mode = S_IFDIR | 0755;
        }
        else
        {
                out->ino = ((uint64_t)n->dirent_sector << 32) | n->dirent_off;
                out->nlink = dir ? 2 : 1;
                if (dir)
                {
                        out->mode = S_IFDIR | 0755;
                        /* 旧工具建的目录项 file_size 可能为 0：沿簇链补算 */
                        out->size = n->file_size ? n->file_size
                                                 : fat12_dir_logical_size(fs, n);
                }
                else
                {
                        out->size = n->file_size;
                        if (n->attr & FAT12_ATTR_READ_ONLY)
                                out->mode = S_IFREG | 0444;
                        else
                                out->mode = S_IFREG | 0644;
                }

                out->atime = fat_datetime_to_unix(n->access_date, 0);
                out->mtime = fat_datetime_to_unix(n->write_date, n->write_time);
                /* FAT 没有 POSIX ctime；用创建时间把三个时间戳都暴露出来 */
                out->ctime = fat_datetime_to_unix(n->create_date, n->create_time);
        }

        out->uid = 0;
        out->gid = 0;
        out->rdev = 0;
        out->blksize = cluster_bytes;
        out->blocks = fat12_alloc_blocks_512(n);
        return 0;
}

/*
 * 目录的逻辑大小（字节，32 的倍数）。
 * 根目录区大小固定；子目录用目录项里记录的 size，旧数据可能为 0，
 * 此时沿 FAT 簇链统计。
 */
static uint32_t fat12_dir_logical_size(struct fat12_priv *fs, struct fat12_node *dir)
{
        uint16_t c;
        uint32_t total = 0;
        uint32_t guard;

        if (dir->is_root)
                return (uint32_t)fs->root_dir_sectors * fs->bytes_per_sector;

        if (dir->file_size != 0)
                return dir->file_size;

        c = dir->start_cluster;
        guard = fs->cluster_count + 2;
        while (c >= 2 && c < FAT12_EOF && guard--)
        {
                total += (uint32_t)fs->sectors_per_cluster * fs->bytes_per_sector;
                c = fat12_read_fat_entry(fs, c);
        }
        return total;
}

/*
 * 读取目录逻辑偏移 off（必须 32 字节对齐）处的原始目录项。
 * 根目录区连续存放；子目录沿簇链定位扇区。
 * 成功返回 0 并填 *out / *out_sector；越过目录结尾或 I/O 失败返回 -1。
 */
static int fat12_dir_read_slot(struct fat12_priv *fs, struct fat12_node *dir,
                               uint32_t off, struct fat12_dirent *out,
                               uint32_t *out_sector)
{
        uint8_t *buf;
        uint32_t sector_idx = off / fs->bytes_per_sector;
        uint32_t slot = (off % fs->bytes_per_sector) / FAT12_DIRENT_SIZE;
        uint32_t sector;
        uint16_t cluster;

        if (dir->is_root)
        {
                if (sector_idx >= fs->root_dir_sectors)
                        return -1;
                sector = fs->root_dir_start + sector_idx;
        }
        else
        {
                cluster = dir->start_cluster;
                if (cluster < 2)
                        return -1;

                /* 沿簇链走到 sector_idx 所在的簇 */
                while (sector_idx >= fs->sectors_per_cluster)
                {
                        sector_idx -= fs->sectors_per_cluster;
                        cluster = fat12_read_fat_entry(fs, cluster);
                        if (cluster >= FAT12_EOF)
                                return -1;
                }
                sector = fat12_cluster_to_sector(fs, cluster) + sector_idx;
        }

        buf = alloc_page();
        if (!buf)
                return -1;

        if (fs->bdev->driver.read(fs->bdev->private_data, sector, buf) < 0)
        {
                free_page(buf);
                return -1;
        }

        *out = *(struct fat12_dirent *)(buf + slot * FAT12_DIRENT_SIZE);
        if (out_sector)
                *out_sector = sector;
        free_page(buf);
        return 0;
}

/*
 * 8.3 目录项名字解码：去空格填充、补 '.'，并按 dir_nt_reserved 的
 * case 标志恢复小写（fat12_set_dirent_case_flags 写入时用的同一约定）。
 * 不支持长文件名（LFN），长名文件返回其 8.3 别名。
 */
static void fat12_dirent_decode_name(const struct fat12_dirent *e, char *dst)
{
        int base_len = 8;
        int ext_len = 3;
        int lower_base = (e->dir_nt_reserved & 0x08) != 0;
        int lower_ext = (e->dir_nt_reserved & 0x10) != 0;
        int j = 0;
        int i;

        while (base_len > 0 && e->dir_name[base_len - 1] == ' ')
                base_len--;
        while (ext_len > 0 && e->dir_ext[ext_len - 1] == ' ')
                ext_len--;

        for (i = 0; i < base_len; i++)
        {
                char c = (char)e->dir_name[i];
                if (lower_base && c >= 'A' && c <= 'Z')
                        c += 'a' - 'A';
                dst[j++] = c;
        }

        if (ext_len > 0)
        {
                dst[j++] = '.';
                for (i = 0; i < ext_len; i++)
                {
                        char c = (char)e->dir_ext[i];
                        if (lower_ext && c >= 'A' && c <= 'Z')
                                c += 'a' - 'A';
                        dst[j++] = c;
                }
        }

        dst[j] = '\0';
}

/*
 * LFN（长文件名）组装。
 * Linux/mtools 写小写名或长名时，会在 8.3 目录项前面放若干 attr=0x0F
 * 的片段：每片 13 个 UCS-2 字符，物理顺序与逻辑顺序相反，seq 的低 5 位
 * 是 1 基的片序号（bit6=1 表示物理第一片），字节 13 是短名校验和。
 */
static const uint8_t fat12_lfn_char_off[13] =
    {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

#define FAT12_LFN_MAX_CHARS VFS_NAME_MAX

static uint8_t fat12_lfn_checksum(const uint8_t *short11)
{
        uint8_t sum = 0;
        int i;

        for (i = 0; i < 11; i++)
                sum = (uint8_t)(((sum & 1) << 7) | (sum >> 1)) + short11[i];
        return sum;
}

/* UCS-2 长名转 ASCII；非可打印 ASCII 字符用 '?' 代替。返回名字长度。 */
static int fat12_lfn_to_ascii(const uint16_t *ucs, char *dst)
{
        int j = 0;
        int i;

        for (i = 0; i < FAT12_LFN_MAX_CHARS && ucs[i] != 0; i++)
        {
                uint16_t c = ucs[i];
                if (j < VFS_NAME_MAX)
                        dst[j++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        dst[j] = '\0';
        return j;
}

/*
 * getdents 的 FAT12 实现。
 * cookie = 目录流字节偏移（0 开始，每次推进一个 32 字节目录项槽位）。
 * 已删除项(0xE5)、卷标项(attr bit3) 在这里过滤；attr=0x0F 的 LFN 片段
 * 聚合成长名（校验和与短名匹配时优先使用）。
 * "." / ".." 是盘上真实目录项，照常返回，由用户程序决定是否显示。
 * 命中返回 1，目录结束返回 0，参数错误返回 -1。
 */
int fat12_readdir(void *node, uint64_t *cookie, struct vfs_dirent *out)
{
        struct fat12_node *dir = (struct fat12_node *)node;
        struct fat12_priv *fs;
        uint32_t dir_size;
        uint32_t off;
        uint32_t sector;
        struct fat12_dirent e;
        /* 正在聚合的 LFN 链（UCS-2 直写按片序号定位，物理逆序无所谓） */
        uint16_t lfn[FAT12_LFN_MAX_CHARS];
        uint8_t lfn_cksum = 0;
        int have_lfn = 0;

        if (!dir || !cookie || !out || !dir->fs_priv)
                return -1;

        fs = dir->fs_priv;
        dir_size = fat12_dir_logical_size(fs, dir);
        off = (uint32_t)*cookie;

        memset(lfn, 0, sizeof(lfn));

        while (off + FAT12_DIRENT_SIZE <= dir_size)
        {
                if (fat12_dir_read_slot(fs, dir, off, &e, &sector) < 0)
                        break; /* 簇链结束或读错误：按目录结束处理 */
                off += FAT12_DIRENT_SIZE;

                /* 0x00 表示该槽及之后全部为空，目录到此结束。
                   cookie 直接推到目录末尾，下次调用立刻返回 0。 */
                if (e.dir_name[0] == 0x00)
                {
                        *cookie = dir_size;
                        return 0;
                }
                if (e.dir_name[0] == 0xE5)
                {
                        have_lfn = 0;
                        continue; /* 已删除（被删的 LFN 片段首字节也是 0xE5） */
                }
                if (e.dir_attr == 0x0F)
                {
                        const uint8_t *raw = (const uint8_t *)&e;
                        int ord = raw[0] & 0x1f;
                        int i;

                        if (raw[0] & 0x40)
                        {
                                /* 物理第一片：开始一条新链 */
                                memset(lfn, 0, sizeof(lfn));
                                have_lfn = 1;
                        }
                        if (have_lfn && ord >= 1)
                        {
                                for (i = 0; i < 13; i++)
                                {
                                        int pos = (ord - 1) * 13 + i;
                                        if (pos < FAT12_LFN_MAX_CHARS)
                                                lfn[pos] =
                                                    raw[fat12_lfn_char_off[i]] |
                                                    ((uint16_t)raw[fat12_lfn_char_off[i] + 1] << 8);
                                }
                                lfn_cksum = raw[13]; /* 各片校验和相同 */
                        }
                        continue;
                }
                if (e.dir_attr & FAT12_ATTR_VOLUME_ID)
                {
                        have_lfn = 0;
                        continue; /* 卷标不是文件 */
                }

                memset(out, 0, sizeof(*out));
                /* 校验和对得上才用长名；对不上只是已删链的残留，回退 8.3 */
                if (have_lfn &&
                    fat12_lfn_checksum((const uint8_t *)&e) == lfn_cksum &&
                    fat12_lfn_to_ascii(lfn, out->name) > 0)
                {
                        /* 名字已由 LFN 填好 */
                }
                else
                        fat12_dirent_decode_name(&e, out->name);
                have_lfn = 0;

                /* ino 规则与 fat12_getattr 一致：(扇区号 << 32) | 槽内偏移 */
                out->ino = ((uint64_t)sector << 32) |
                           (uint16_t)((off - FAT12_DIRENT_SIZE) % fs->bytes_per_sector);
                out->off = off;
                out->type = (e.dir_attr & FAT12_ATTR_DIRECTORY) ? DT_DIR : DT_REG;

                *cookie = off;
                return 1;
        }

        *cookie = off;
        return 0;
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
        // uint8_t sector_buf[512];
        uint8_t *sector_buf = alloc_page();

        // 只读文件不能写
        if (fnode->attr & FAT12_ATTR_READ_ONLY)
        {
                free_page(sector_buf);
                return -1;
        }

        // 目录不能写
        if (fnode->attr & FAT12_ATTR_DIRECTORY)
        {
                free_page(sector_buf);
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
                        free_page(sector_buf);
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
                                free_page(sector_buf);
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
                        free_page(sector_buf);
                        return -1;
                }

                // 修改字节
                sector_buf[byte_off] = src[i];

                // 写回扇区
                if (fs->bdev->driver.write(fs->bdev->private_data, sector, sector_buf) < 0)
                {
                        *out_len = i;
                        file->pos += i;
                        free_page(sector_buf);
                        return -1;
                }

                bytes_written++;
                current_offset++;
        }

        // file->pos += bytes_written;
        *out_len = bytes_written;
        free_page(sector_buf);
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
        // uint8_t sector_buf[512];
        uint8_t *sector_buf = alloc_page();
        uint64_t bytes_read = 0;
        uint32_t offset = file->pos;

        // 如果已经到文件末尾
        if (offset >= fnode->file_size)
        {
                *out_len = 0;
                free_page(sector_buf);
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
                        free_page(sector_buf);
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
                        free_page(sector_buf);
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
        free_page(sector_buf);
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
                memset(node, 0, sizeof(*node));
                node->is_root = 1;
                node->fs_priv = fs_p;
                node->attr = FAT12_ATTR_DIRECTORY;
                node->file_size = fs_p->root_dir_sectors * fs_p->bytes_per_sector;
                node->start_cluster = 0;
                *out_node = node;
                return (int)node->file_size;
        }

        // 从根目录开始查找
        uint32_t current_sector = fs_p->root_dir_start;
        uint32_t current_size = fs_p->root_entries * 32;

        // printk("fat12_lookup: path='%s' root_dir_start=%d root_entries=%d\n",
        //        path, fs_p->root_dir_start, fs_p->root_entries);

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
                uint32_t dent_sec = 0;
                uint16_t dent_off = 0;
                ret = find_in_dir(fs_priv, current_sector, current_size, filename, &entry,
                                  &dent_sec, &dent_off);
                if (ret < 0)
                {
                        // printk("fat12_lookup: NOT FOUND '%s' sector=%d size=%d\n",
                        //        filename, current_sector, current_size);
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
                        // FAT 子目录的 size 可能为 0（旧数据或某些实现），
                        // 但目录至少占一个簇，按簇链计算实际大小
                        if (current_size == 0 && (entry.dir_attr & FAT12_ATTR_DIRECTORY))
                        {
                                current_size = fs_p->sectors_per_cluster * 512;
                        }
                        path += i + 1;
                        continue;
                }

                // 找到了
                node = (struct fat12_node *)alloc_page();
                if (!node)
                {
                        return -1;
                }

                memset(node, 0, sizeof(*node));
                node->is_root = 0;
                node->fs_priv = fs_priv;
                node->start_cluster = entry.dir_first_cluster_low;
                node->file_size = entry.dir_file_size;
                node->attr = entry.dir_attr;
                memcpy(node->name, entry.dir_name, 11);
                node->dirent_sector = dent_sec;
                node->dirent_off = dent_off;
                node->create_time = entry.dir_create_time;
                node->create_date = entry.dir_create_date;
                node->write_time = entry.dir_write_time;
                node->write_date = entry.dir_write_date;
                node->access_date = entry.dir_last_access_date;
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
                       const char *filename, struct fat12_dirent *out,
                       uint32_t *out_sector, uint16_t *out_off)
{
        uint8_t *buf = alloc_page();
        if (!buf)
                return -1;

        int sector_count = dir_size / 512;
        if (dir_size % 512)
                sector_count++;

        for (int s = 0; s < sector_count; s++)
        {
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
                                continue;
                        if (e->dir_attr == 0x08)
                                continue;

                        if (match_dos_name(e, filename))
                        {
                                *out = *e;
                                if (out_sector)
                                        *out_sector = start_sector + s;
                                if (out_off)
                                        *out_off = (uint16_t)(i * FAT12_DIRENT_SIZE);
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