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

#define PG_FLAG_RESERVED 1 << 0

typedef unsigned long phys_addr_t;

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
        struct page *head;
        struct page *tail;
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
#endif