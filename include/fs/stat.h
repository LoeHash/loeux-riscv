#ifndef _INC_STAT_
#define _INC_STAT_

/* 用户库 ulib.h 已提供整型；内核侧走 stdint。 */
#ifndef _INC_USER_ULIB
#include <stdint.h>
#endif

/*
 * POSIX/Unix struct stat ABI（用户态与内核 copyout 共用同一布局）。
 *
 * 本内核没有 Unix inode 对象；VFS 的「文件身份」由各文件系统的
 * 私有节点（如 fat12_node）承担，再映射到 st_ino / st_dev。
 */

typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t mode_t;
typedef uint32_t nlink_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef int64_t off_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;
typedef int64_t time_t;

/* 文件类型（st_mode & S_IFMT） */
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000

#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000

#define S_IRWXU 0000700
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRWXG 0000070
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IRWXO 0000007
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

struct stat
{
        dev_t st_dev;         /* 所在文件系统的设备号 */
        ino_t st_ino;         /* 文件系统内唯一编号 */
        mode_t st_mode;       /* 类型 + 权限 */
        nlink_t st_nlink;     /* 硬链接数 */
        uid_t st_uid;         /* 所有者 */
        gid_t st_gid;         /* 所属组 */
        dev_t st_rdev;        /* 特殊文件对应的设备号 */
        off_t st_size;        /* 逻辑大小（字节） */
        blksize_t st_blksize; /* 首选 I/O 块大小 */
        blkcnt_t st_blocks;   /* 已分配的 512 字节块数 */
        time_t st_atime;      /* 最后访问（秒，Unix epoch） */
        time_t st_mtime;      /* 最后内容修改 */
        time_t st_ctime;      /* 最后状态变更（或最接近的替代） */
};

#endif
