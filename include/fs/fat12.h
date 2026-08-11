// fat12.h
#ifndef _INC_FAT12_
#define _INC_FAT12_
#include <type.h>
#include <vfs.h>

#define FAT12_SECTOR_SIZE 512
#define FAT12_DIRENT_SIZE 32
#define FAT12_EOF 0xFF8
#define FAT12_EOF_MASK 0x0FFF

// 文件属性
#define FAT12_ATTR_READ_ONLY 0x01
#define FAT12_ATTR_HIDDEN 0x02
#define FAT12_ATTR_SYSTEM 0x04
#define FAT12_ATTR_VOLUME_ID 0x08
#define FAT12_ATTR_DIRECTORY 0x10
#define FAT12_ATTR_ARCHIVE 0x20

// 1. BPB (BIOS Parameter Block) - 引导扇区
struct fat12_bpb
{
        // 跳转指令 (3字节)
        uint8_t bs_jmp_boot[3];
        uint8_t bs_oem_name[8];

        // BPB 部分 (25字节)
        uint16_t bpb_bytes_per_sector;    // 每扇区字节数 (512)
        uint8_t bpb_sectors_per_cluster;  // 每簇扇区数 (1,2,4,8,16,32,64)
        uint16_t bpb_reserved_sectors;    // 保留扇区数 (通常 1)
        uint8_t bpb_num_fats;             // FAT 表数量 (通常 2)
        uint16_t bpb_root_entries;        // 根目录项数 (FAT12 通常 224)
        uint16_t bpb_total_sectors;       // 总扇区数 (如果为 0，用 bpb_total_sectors_large)
        uint8_t bpb_media_descriptor;     // 介质描述符 (0xF0, 0xF8 等)
        uint16_t bpb_sectors_per_fat;     // 每个 FAT 表占用的扇区数
        uint16_t bpb_sectors_per_track;   // 每磁道扇区数
        uint16_t bpb_num_heads;           // 磁头数
        uint32_t bpb_hidden_sectors;      // 隐藏扇区数
        uint32_t bpb_total_sectors_large; // 总扇区数 (如果 bpb_total_sectors == 0)

        // 扩展引导 (12字节)
        uint8_t bs_drv_num;          // 驱动器号 (0x80)
        uint8_t bs_reserved1;        // 保留
        uint8_t bs_boot_sig;         // 扩展引导签名 (0x29)
        uint32_t bs_vol_id;          // 卷序列号
        uint8_t bs_vol_label[11];    // 卷标 (11字节，不足补空格)
        uint8_t bs_file_sys_type[8]; // 文件系统类型 ("FAT12   ")

        // 签名 (2字节)
        uint8_t signature[2]; // 0x55, 0xAA
} __attribute__((packed));

// ============================================================
// 2. 目录项 (32 字节)
// ============================================================
struct fat12_dirent
{
        uint8_t dir_name[8];             // 文件名 (8字节，不足补空格)
        uint8_t dir_ext[3];              // 扩展名 (3字节，不足补空格)
        uint8_t dir_attr;                // 文件属性
        uint8_t dir_nt_reserved;         // NT 保留 (Windows 用)
        uint8_t dir_create_time_tenth;   // 创建时间 (1/100 秒)
        uint16_t dir_create_time;        // 创建时间 (时:分:秒)
        uint16_t dir_create_date;        // 创建日期 (年:月:日)
        uint16_t dir_last_access_date;   // 最近访问日期
        uint16_t dir_first_cluster_high; // 首簇号高 16 位 (FAT12 为 0)
        uint16_t dir_write_time;         // 修改时间
        uint16_t dir_write_date;         // 修改日期
        uint16_t dir_first_cluster_low;  // 首簇号低 16 位 (FAT12 用这个)
        uint32_t dir_file_size;          // 文件大小 (字节)
} __attribute__((packed));

struct fat12_priv
{
        // 从 BPB 读出来的
        uint16_t bytes_per_sector;
        uint8_t sectors_per_cluster;
        uint16_t reserved_sectors;
        uint8_t fat_count;
        uint16_t root_entries;
        uint16_t sectors_per_fat;
        uint16_t total_sectors;

        // 计算出来的
        uint32_t fat_start;        // FAT 表起始扇区
        uint32_t root_dir_start;   // 根目录起始扇区
        uint32_t root_dir_sectors; // 根目录占用的扇区数
        uint32_t data_start;       // 数据区起始扇区
        uint32_t cluster_count;    // 总簇数

        uint8_t *fat_table; // 指向 FAT 表数据 (sectors_per_fat × bytes_per_sector 字节)

        // 块设备 (用于读写)
        struct block_device *bdev;
};

// 查找结果
struct fat12_node
{
        uint16_t start_cluster;
        uint32_t file_size;
        uint8_t attr;
        uint8_t name[11];
        int is_root;
        struct fat12_priv *fs_priv;
};

// 挂载函数: 读 BPB，校验，构造 fat12_priv
void *fat12_mount(struct block_device *bdev);
int fat12_lookup(void *fs_priv, const char *rel_path, void **out_node);
void fat12_free_node(void *out_node);
int fat12_open(void *node, struct file *file, int flags);
int fat12_read(struct file *file, void *buf, uint64_t count, uint64_t *out_len);
int fat12_write(struct file *file, const void *buf, uint64_t count, uint64_t *out_len);
int fat12_close(struct file *file);
/// @brief mode = 0 文件 1 目录
int fat12_create(void *fs_priv, const char *rel_path, int mode);
extern struct file_operation fat12_ops;

#endif