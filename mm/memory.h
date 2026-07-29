// 关于内存:
// 系统调用不切换页表（只改变特权级和栈）
#ifndef _INC_MEMORY_
#include "../include/stdint.h"
#define _INC_MEMORY_
#define NR_BANKS_SIZE 16
#define PG_4K_SHIFT 12
#define PG_4K_SIZE (1 << 12)
#define KERNEL_START 0x80200000
#define MEMORY_START 0x80000000
#define PHY_TO_PAGE(phys) \
        (&((struct page *)_phy_start)[((phys_addr_t)(phys) - MEMORY_START) >> PG_4K_SHIFT])

#define PRINT_PAGE_INFO(p, label)                                                          \
        do                                                                                 \
        {                                                                                  \
                struct page *pp = PHY_TO_PAGE((phys_addr_t)p);                             \
                printk("%s: p=%p page=%p paddr=%#x flags=%#x ref=%d prev=%p next=%p %s\n", \
                       label, p, pp, pp->paddr, pp->flags, pp->refcount,                   \
                       pp->prev, pp->next,                                                 \
                       (pp->flags & PG_FLAG_USED) ? "USED" : "FREE");                      \
        } while (0)

#define PG_FLAG_FREE 0
#define PG_FLAG_RESERVED 1 << 0
#define PG_FLAG_USED 1 << 1

#define MAX_VA (1ULL << (38)) // 使用 sv39能达到的最高的虚拟地址，实际上应该-1

typedef unsigned long phys_addr_t;
typedef unsigned long vir_addr_t;

/// @brief 表示一页的管理信息
struct page
{
        phys_addr_t paddr;
        int32_t refcount;
        uint64_t flags;
        struct page *prev;
        struct page *next;
};

struct gloal_memory_descriptor
{
        struct page *pg;
        struct page *kernel_head; // 保留节点的头
        struct page *kernel_tail; // 保留节点的尾
        struct page *fdt_head;    // fdt设备树保留节点的头
        struct page *fdt_tail;    // fdt设备树保留节点的尾
        struct page *free_head;   // 空闲节点的头
        struct page *free_tail;   // 空闲节点的尾
        uint64_t page_length;
};

struct bank
{
        phys_addr_t base;
        phys_addr_t size;
};

struct memory_info
{
        struct bank banks[NR_BANKS_SIZE];
        int nr_banks;
};

extern struct memory_info mem_info;
extern struct gloal_memory_descriptor gmd;
extern uint64_t MEMORY_SIZE;
int detect_memory_info(const char *name, int depth,
                       void *node_ptr,
                       void *data);
void init_memory();
void *alloc_page();
int free_page(void *pa);
#endif