#ifndef _INC_VFS_
#define _INC_VFS_
#include <type.h>
#include <block_device.h>

// 文件打开标志
#define FS_O_READ 0x01   // bit 0
#define FS_O_WRITE 0x02  // bit 1
#define FS_O_RW 0x03     // READ | WRITE
#define FS_O_EXEC 0x04   // bit 2
#define FS_O_CREAT 0x08  // bit 3
#define FS_O_TRUNC 0x10  // bit 4
#define FS_O_APPEND 0x20 // bit 5

// 目录权限
#define FS_MODE_READ 0400
#define FS_MODE_WRITE 0200
#define FS_MODE_EXEC 0100
#define ROOT_FD 0

#define MAX_MOUNT_NUM 16
#define MAX_FD_NUM 256
#define DEV_PATH_PREFIX "/dev/"
#define DEV_PATH_PREFIX_LEN 5

typedef int fd_t;
typedef uint64_t fs_off_t;

struct mount_entry
{
        char mount_point[32];
        struct block_device *device;
        struct file_operation *fs_ops;
        void *fs_priv;
};

struct file
{
        struct mount_entry *mnt; // 这个文件属于哪个挂载点
        uint64_t pos;            // 文件位置信息
        uint64_t offset;         // 当前读写位置 （已弃用）
        uint64_t size;           // 当前读写位置
        int flags;               // 打开时的标志
        void *private;
        int type; // 0 块设备 1 字符设备
};

struct vfs_node
{
        struct mount_entry *mount;
        void *private;
        uint64_t size;
        uint8_t is_dir;
        int refcount;
        struct vfs_node *next; // 链表指针
};

// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// 所有的文件系统都不应直接修改file的任何属性，原因如下:
// 1. 语义不明
// 2. 与vfs层逻辑混乱
// 3. seeking...
// lookup 返回值语义
// 返回值是文件大小
struct file_operation
{
        void *(*fs_mount)(struct block_device *bdev);
        int (*fs_lookup)(void *fs_priv, const char *rel_path, void **out_node);
        void (*fs_free_node)(void *out_node);
        int (*fs_open)(void *node, struct file *file, int flags);
        int (*fs_read)(struct file *file, void *buf, uint64_t count, uint64_t *out_len);
        int (*fs_write)(struct file *file, const void *buf, uint64_t count, uint64_t *out_len);
        int (*fs_close)(struct file *file);
        /// @brief mode = 0 文件 1 目录
        int (*fs_create)(void *fs_priv, const char *rel_path, int mode);
};

typedef enum
{

        FAT12,
        FAT32,
        FS_RAMFS
} FSTYPE;

extern struct mount_entry mount_points[MAX_MOUNT_NUM];
void init_vfs(void);
void init_vfs_std();
int vfs_create(const char *path, int is_dir);
int vfs_close(int fd);
int64_t vfs_write(int fd, const void *buf, uint64_t count);
int64_t vfs_read(int fd, void *buf, uint64_t count);
int vfs_open(const char *path, int flags);
int vfs_seek(int fd, uint64_t offset);
struct vfs_node *vfs_lookup(const char *path);
int vfs_mount(char *mount_path, struct block_device *bdev, FSTYPE type);
void print_mount_table(void);
void test_fat12_operations(void);
#endif
