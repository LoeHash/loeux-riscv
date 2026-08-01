// 关于内存:
// 系统调用不切换页表（只改变特权级和栈）
#ifndef _INC_MEMORY_
#define _INC_MEMORY_
#include <stdint.h>
#include <memlayout.h>
#include <type.h>
#define _INC_MEMORY_
#define NR_BANKS_SIZE 16
#define PG_4K_SHIFT 12
#define PG_4K_SIZE (1 << 12)

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

struct memory_info
{
        struct bank banks[NR_BANKS_SIZE];
        int nr_banks;
};

extern struct memory_info mem_info;
extern struct gloal_memory_descriptor gmd;
extern uint64_t MEMORY_SIZE;
extern uint8_t memory_init_status;
extern spinlock_t memory_init_lock;

int detect_memory_info(const char *name, int depth,
                       void *node_ptr,
                       void *data);
void init_memory();
void *alloc_page();
int free_page(void *pa);
#endif