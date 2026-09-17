#ifndef _INC_USER_UFILE_H
#define _INC_USER_UFILE_H
#include <utype.h>
#include <stat.h>
#include <dirent.h>
#include <ustring.h>

///////////////////////////////MACROS///////////////////////////////
/*
 * open flags ABI：必须与内核 include/fs/vfs.h 的定义逐值一致，
 * 这些值经 ecall 原样传给 sys_open / vfs_open。
 */
#define O_RDONLY (0x0000)
#define O_WRONLY (0x0001)
#define O_RDWR (0x0002)
#define O_ACCMODE (0x0003)
#define O_CREAT (0x0004)     // bit 2
#define O_EXCL (0x0008)      // bit 3
#define O_TRUNC (0x0010)     // bit 4
#define O_APPEND (0x0020)    // bit 5
#define O_DIRECTORY (0x0040) // bit 6

/* 兼容别名 */
#define O_READ O_RDONLY
#define O_WRITE O_WRONLY
#define O_RW O_RDWR
///////////////////////////////MACROS END///////////////////////////////

#endif