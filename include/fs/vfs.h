#ifndef _INC_VFS_
#define _INC_VFS_
#include <type.h>
#include <block_device.h>
#include <sleeplock.h>
#include <stat.h>
#include <dirent.h>

/*
 * 文件系统向 VFS 报告的通用元数据。各 FS 填自己能提供的字段，
 * 缺失项置 0。VFS 再补 st_dev 并转换成用户可见的 struct stat。
 */
struct vfs_kstat
{
        uint64_t ino;
        uint32_t mode;
        uint32_t nlink;
        uint32_t uid;
        uint32_t gid;
        uint64_t rdev;
        uint64_t size;
        uint64_t blksize;
        uint64_t blocks;
        int64_t atime;
        int64_t mtime;
        int64_t ctime;
};

/*
 * 文件系统向 VFS 报告的一条通用目录项。
 * 名字解码、隐藏项（已删除/LFN/卷标）过滤都由具体 FS 完成，
 * VFS 只负责翻译成用户 ABI struct dirent。
 */
struct vfs_dirent
{
        uint64_t ino;                /* inode 号（同 vfs_kstat.ino 规则） */
        uint64_t off;                /* 下一条目的 cookie，FS 私有语义 */
        uint8_t type;                /* DT_* 类型 */
        char name[VFS_NAME_MAX + 1]; /* 解码后的文件名 */
};

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

/// @brief 文件模式
/*
        从低位开始算
                0x1: 表示可读
                0x2: 表示可写
                0x4: 表示可执行
                0x8: 表示目录
                0x10及以后的位: 表示其他
*/
typedef int file_mode_t;

/// @brief 检查文件模式是否具有某权限
#define HAS_PERM(fm, offset) (((fm) >> (offset)) & 1U)
#define SETPERM(fm, offset) ((fm) |= (1U << (offset)))
#define IRPERM 0
#define IWPERM 1
#define IXPERM 2
#define ISDIR 3

/// @brief 通用文件属性（跨文件系统）
/// VFS 层使用此结构传递文件属性，各文件系统自行转换为内部属性格式。
/// 例如 FAT12 通过 fat12_attr_from_generic() 转换为 FAT 属性字节。
typedef struct
{
        int is_dir;   // 是否为目录
        int readable; // 可读
        int writable; // 可写
} file_attr_t;

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
        // 引用计数：fork 后父子进程共享同一个 struct file（共享读写位置），
        // 每次共享 refcount++，每次 close refcount--，归 0 才真正释放底层资源。
        // 使用原子操作修改，避免 SMP 下父子进程同时 close 时的竞态。
        int refcount;

        // 串行化对该 file 的访问：保护 pos（读写位置）以及底层 read/write 的原子性。
        // fork 后父子共享同一个 struct file，因此也共享这把锁——父子并发写同一
        // 文件（如 stdout）时会被这把锁排队，避免输出交错。
        // 必须用 init_sleeplock 初始化（在 vfs_open 中完成），否则 wait_queue
        // 未形成循环链表，release 时 wakeup 会解引用 NULL 触发缺页。
        sleeplock_t flk;
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
        int (*fs_create)(void *fs_priv, const char *rel_path, file_attr_t attr);
        /// @brief 判断节点是否为目录
        int (*fs_is_dir)(void *node);
        /// @brief 读取节点元数据（POSIX fstat/stat 的 FS 侧实现）
        int (*fs_getattr)(void *node, struct vfs_kstat *out);
        /// @brief 读取一个目录项（getdents 的 FS 侧实现）
        /// *cookie 是 FS 私有迭代位置（FAT12 为目录流字节偏移），
        /// 命中时填 out、推进 *cookie 并返回 1；目录结束返回 0；出错返回 -1。
        int (*fs_readdir)(void *node, uint64_t *cookie, struct vfs_dirent *out);
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
int vfs_create(const char *path, file_attr_t attr);
int vfs_close(int fd);
int64_t vfs_write(int fd, const void *buf, uint64_t count);
int64_t vfs_read(int fd, void *buf, uint64_t count);
int vfs_open(const char *path, int flags);
int vfs_seek(int fd, uint64_t offset);
int vfs_fstat(int fd, struct stat *st);
int64_t vfs_getdents(int fd, void *buf, uint64_t count);
struct vfs_node *vfs_lookup(const char *path);
int vfs_mount(char *mount_path, struct block_device *bdev, FSTYPE type);
void print_mount_table(void);
void test_fat12_operations(void);

// 释放对 struct file 的一次引用：refcount-- 归 0 时真正调用底层 fs_close
// 并 free_page。供 vfs_close 与 task 退出清理路径使用。
// 调用者必须已经把 file 从 ofile[] 摘除（避免 double close）。
void file_close(struct file *file);
#endif
