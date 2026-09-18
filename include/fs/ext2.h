#ifndef _INC_EXT2_
#define _INC_EXT2_
#include <type.h>
#include <vfs.h>

struct block_device;
struct super_block;

/* ext2 on-disk magic */
#define EXT2_MAGIC 0xEF53

/* ext2 revision levels */
#define EXT2_REV_LEVEL_0 0
#define EXT2_REV_LEVEL_1 1

/* supported maximum block size (slab allocator cap) */
#define EXT2_MAX_BLOCK_SIZE 4096

/* inode size for rev 0 (default) */
#define EXT2_INODE_SIZE_DEFAULT 128

/* file type field in directory entries */
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR 2
#define EXT2_FT_SYMLINK 7

/* inode mode bits relevant to ext2 (overlap with VFS S_IF*) */
#define EXT2_S_IFREG 0100000
#define EXT2_S_IFDIR 0040000
#define EXT2_S_IFLNK 0120000

/* number of block pointers in an inode (12 direct + indirects) */
#define EXT2_N_BLOCKS 15
#define EXT2_IND_BLOCK 12
#define EXT2_DIND_BLOCK 13
#define EXT2_TIND_BLOCK 14

/* reserved inode numbers */
#define EXT2_ROOT_INO 2

/* 文件系统私有信息（从超级块解析） */
struct ext2_fs_priv
{
        struct block_device *bdev;

        uint32_t block_size;
        uint32_t inodes_per_group;
        uint32_t blocks_per_group;
        uint32_t inode_size;
        uint32_t num_groups;
        uint32_t inodes_count;
        uint32_t blocks_count;
        uint32_t free_inodes;
        uint32_t free_blocks;
        uint32_t first_data_block;
        uint32_t ptrs_per_block; /* block_size / 4 */

        /* 缓存的超级块字段，写回时需要保持一致 */
        uint32_t s_state;
        uint32_t s_rev_level;
};

/*
 * ext2 inode 私有数据
 *
 * 缓存磁盘 inode 的完整副本，避免每次读写都重新读盘。
 * 修改时直接修改 disk_inode 并通过 ext2_inode_save 回写。
 */
struct ext2_inode_priv
{
        uint32_t ino;     /* 1-based inode number */
        uint32_t i_uid;
        uint32_t i_gid;
        uint32_t i_size;
        uint32_t i_mode;
        uint32_t i_links_count;
        uint32_t i_blocks; /* 占用的 512 字节扇区数（advisory，e2fsck 会校验） */
        uint32_t i_atime;
        uint32_t i_ctime;
        uint32_t i_mtime;
        uint32_t i_block[EXT2_N_BLOCKS];
};

/* 注册 ext2 文件系统到 VFS */
void init_ext2(void);

#endif
