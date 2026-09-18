#ifndef MALLOC_H
#define MALLOC_H

#include <utype.h>

/* ---- 对齐 ---- */
#define ALIGN        16
#define ALIGN_UP(x)  (((x) + (ALIGN - 1)) & ~(ALIGN - 1))

/* ---- 用户态自旋锁 ---- */
typedef struct { volatile int locked; } umutex;

/* ---- 块头 ---- */
struct block {
    size_t        size;   /* payload 字节数，已 16 对齐 */
    int           free;   /* 1=空闲 0=已用 */
    struct block *next;   /* 物理相邻下一块 */
    struct block *prev;   /* 物理相邻上一块 */
    uint64_t      _pad[2];/* 凑满 48 字节，保证 payload 16 对齐 */
};

#define BLK_HDR  (sizeof(struct block))   /* 48 */

/* ---- 标准接口 ---- */
void *malloc(size_t n);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t n);

#endif