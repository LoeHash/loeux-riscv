#ifndef _INC_USER_STAT_H
#define _INC_USER_STAT_H

#include <utype.h>

/*
 * POSIX/Unix struct stat ABI（用户态与内核 copyout 共用同一布局）。
 */

/*
 * 注意：不能写成 typedef uint64_t dev_t; 因为 lcc 把 uint64_t/dev_t 等
 * 都硬编码为类型名（见 lcc.c is_typedef_name），两个类型名连续出现会导致
 * "expected expression"。这里直接用关键字底层类型。
 */
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int mode_t;
typedef unsigned int nlink_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef long off_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef long time_t;

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
