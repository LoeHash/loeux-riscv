#ifndef _INC_DIRENT_
#define _INC_DIRENT_

/* 用户库 ulib.h 已提供整型；内核侧走 stdint。 */
#ifndef _INC_USER_ULIB
#include <stdint.h>
#endif

/*
 * getdents 的目录项 ABI（内核 copyout 与用户程序共用同一布局）。
 *
 * 风格来自 Linux getdents64：d_ino / d_off / d_reclen / d_type / d_name。
 * 本内核不做变长记录——d_reclen 恒为 sizeof(struct dirent)，
 * 用户态按固定步长遍历返回缓冲区即可：
 *
 *     struct dirent *d;
 *     for (int off = 0; off < n; off += d->d_reclen)
 *         d = (struct dirent *)(buf + off);
 *
 * 内核与用户态均用同一版 riscv64 gcc 编译，填充规则一致；
 * 所有字段本身也都是自然对齐的。
 */

#define VFS_NAME_MAX 255 /* 文件名最大长度（不含结尾 '\0'） */

/* d_type 取值（与 Linux d_type 约定一致） */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

/* S_IFMT 类型位 <-> d_type 互转（mode 高 12 位右移 12 即得到 DT_*） */
#define IFTODT(mode) (((mode) & 0170000) >> 12)
#define DTTOIF(type) ((type) << 12)

struct dirent
{
        uint64_t d_ino;                /* 文件系统内唯一编号（同 st_ino） */
        int64_t d_off;                 /* 下一条目的不透明 cookie */
        uint16_t d_reclen;             /* 本条记录长度，恒为 sizeof(struct dirent) */
        uint8_t d_type;                /* DT_* 文件类型 */
        char d_name[VFS_NAME_MAX + 1]; /* 文件名，NUL 结尾 */
};

#endif
