#ifndef _INC_FAT32_
#define _INC_FAT32_
#include <type.h>
#include <vfs.h>

struct block_device;
struct super_block;

/* FAT32 on-disk magic */
#define FAT32_SECTOR_SIZE 512
#define FAT32_DIRENT_SIZE 32
#define FAT32_EOF_MARK 0x0FFFFFF8
#define FAT32_EOF_MASK 0x0FFFFFF8 /* >= 该值视为链尾 */
#define FAT32_FREE_MARK 0x00000000
#define FAT32_BAD_MARK 0x0FFFFFF7

/* 文件属性 */
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN 0x02
#define FAT32_ATTR_SYSTEM 0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20
#define FAT32_ATTR_LONG_NAME 0x0F

/* 文件系统私有信息（从 BPB 解析） */
struct fat32_fs_priv
{
        struct block_device *bdev;

        uint32_t bytes_per_sector;
        uint32_t sectors_per_cluster;
        uint32_t reserved_sectors;
        uint32_t num_fats;
        uint32_t fat_sectors;
        uint32_t total_sectors;

        /* 计算出来的布局 */
        uint32_t fat_start;   /* 第一个FAT起始扇区 */
        uint32_t data_start;  /* 数据区起始扇区 */
        uint32_t root_cluster;
        uint32_t cluster_count; /* 数据区簇数 */
};

/*
 * FAT32 inode 私有数据
 *
 * 一个 inode 对应一个目录项。
 * dir_cluster/dirent_off 定位该文件在父目录中的32字节目录项，
 * 用作 st_ino 与回写 size/时间戳的依据。
 * 根目录没有父目录项（is_root = 1）。
 */
struct fat32_inode_priv
{
        uint32_t first_cluster; /* 首簇号, 0 表示空文件 */
        uint32_t attr;          /* FAT属性字节 */
        uint32_t dir_cluster;   /* 目录项所在目录数据的簇号, 根目录为0 */
        uint32_t dirent_off;    /* 目录项在该簇内的字节偏移(32对齐) */
        uint8_t is_root;
};

/* 注册 fat32 文件系统到 VFS（内部构造 filesystem 对象） */
void init_fat32(void);

#endif
