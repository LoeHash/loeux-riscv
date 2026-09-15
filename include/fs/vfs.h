#ifndef _INC_VFS_
#define _INC_VFS_
#include <type.h>
#include <block_device.h>
#include <sleeplock.h>
#include <stat.h>
#include <dirent.h>

#define MAY_READ 0x01
#define MAY_WRITE 0x02
#define MAY_EXEC 0x04

/*
 * inode mode
 *
 * 高位：文件类型
 * 低位：文件权限
 */

/* file type */
#define S_IFMT 0170000
#define S_IFREG 0100000 /* regular file */
#define S_IFDIR 0040000 /* directory */
#define S_IFCHR 0020000 /* character device */
#define S_IFBLK 0060000 /* block device */
#define S_IFLNK 0120000 /* symbolic link */

/* owner permission */
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100

/* group permission */
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010

/* other permission */
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001

#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRWXG (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IRWXO (S_IROTH | S_IWOTH | S_IXOTH)

/* access mode */
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002

#define O_ACCMODE 0x0003

/* open behavior */
#define O_CREAT 0x0004
#define O_EXCL 0x0008
#define O_TRUNC 0x0010
#define O_APPEND 0x0020
#define O_DIRECTORY 0x0040

#define VFS_FS_NAME_MAX 64
#define VFS_MAX_PATH_LEN 128 // 最长就是128的path描述
#define VFS_MAX_FD_NUM 64

struct filesystem;
struct super_block;
struct mount;
struct inode;
struct file;
struct file_operations;

struct filesystem_registry
{
        struct filesystem *head;

        struct spinlock lock;
};

typedef struct filesystem_registry filesystem_registry_t;

/// @note fs和sb的关系在于: 一个是描述数据的组织方式, 一个是描述该以什么样的方式读取
/*
 * 文件系统类型
 */
struct filesystem
{
        const char name[VFS_FS_NAME_MAX];

        int (*get_super)(struct filesystem *fs,
                         struct block_device *dev,
                         struct super_block **sb);

        void (*kill_sb)(struct super_block *sb);

        struct file_operations *fops;
        struct inode_operations *iops;

        struct filesystem *next;
};

typedef struct filesystem filesystem_t;

/*
 * 一个具体的挂载点
 * /media/loe -> 某个 FAT12 super_block
 */
struct mount
{
        char path[VFS_MAX_PATH_LEN];

        struct super_block *sb;

        struct mount *parent;
        struct mount *child;
        struct mount *next;
};

typedef struct mount mount_t;

struct mount_table
{
        struct mount *root;

        struct spinlock lock;
};

typedef struct mount_table mount_table_t;

/*
 * 一个具体的文件系统实例
 *
 * 如：loeux.img 被作为 FAT12 挂载到 /media/loe
 */
struct super_block
{
        struct filesystem *fs;
        struct block_device *dev;

        struct inode *root;

        void *private;
};

typedef struct super_block super_block_t;

struct inode
{
        struct super_block *sb;

        uint64_t ino;

        uint32_t mode;

        uint32_t uid;
        uint32_t gid;

        uint64_t size;

        struct inode_operations *iops;
        struct file_operations *fops;

        void *private;

        uint32_t refcount;
};

typedef struct inode inode_t;

/*
 * 一次 open 对应的 open file description
 *
 * fork / dup 后可以被多个 fd 共享。
 */
struct file
{
        struct inode *inode;

        uint64_t pos;
        uint32_t flags;

        struct file_operations *fops;

        void *private;

        uint32_t refcount;

        struct sleeplock slk;
};

typedef struct file file_t;

/*
 * 进程自己的 fd 表
 */
struct fd_table
{
        struct file *files[VFS_MAX_FD_NUM];

        struct spinlock lock;
};

/*
 * inode 层操作
 *
 * 操作“文件 / 目录对象本身”
 */
struct inode_operations
{
        int (*lookup)(struct inode *dir,
                      const char *name,
                      struct inode **inode);

        int (*create)(struct inode *dir,
                      const char *name,
                      uint32_t mode,
                      struct inode **inode);

        int (*mkdir)(struct inode *dir,
                     const char *name,
                     uint32_t mode);

        int (*rmdir)(struct inode *dir,
                     const char *name);

        int (*unlink)(struct inode *dir,
                      const char *name);

        int (*readdir)(struct inode *dir,
                       uint64_t *offset,
                       struct vfs_dirent *dirent);

        int (*getattr)(struct inode *inode,
                       struct vfs_kstat *stat);
};

/*
 * file 层操作
 *
 * 操作“已经 open 的文件”
 * 对于file 和 file_operations:
 * process
 * │
 * └── fd_table[3]
 *         │
 *         ▼
 *      file
 *      ├── inode ─────► hello.txt
 *      ├── pos = 0
 *      ├── flags
 *      └── fops ──────► fat12_file_ops
 *                             │
 *                             ├── read  → fat12_read
 *                             ├── write → fat12_write
 *                             ├── seek  → fat12_seek
 *                             └── close → fat12_close
 */
struct file_operations
{
        int64_t (*read)(struct file *file,
                        void *buf,
                        uint64_t count);

        int64_t (*write)(struct file *file,
                         const void *buf,
                         uint64_t count);

        int64_t (*seek)(struct file *file,
                        int64_t offset,
                        int whence);

        int (*close)(struct file *file);
};

typedef struct inode_operations inode_operations_t;
typedef struct file_operations file_operations_t;

int vfs_mount(struct block_device *dev,
              const char *target,
              const char *fs_type);
#endif
