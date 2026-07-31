#ifndef _INC_VM_
#define _INC_VM_
#include "../include/stdint.h"
#include "memory.h"
#include "../kernel/spinlock.h"

#define PTE_V (1L << 0) // valid, 默认分配的所有pte都会加上
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4)    // user can access
#define PTE_G (1L << 5)    // user can access
#define PTE_A (1L << 6)    // is accessed
#define PTE_D (1L << 7)    // is writen
#define PTE_C (1L << 8)    // is cow pte?
#define PTE_SAVE (1L << 9) // RESERVED JUST FOR NOW.......
#define PGSHIFT 12
#define PXMASK 0x1FF // 9 bits
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va) ((((uint64_t)(va)) >> PXSHIFT(level)) & PXMASK)
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PA2PTE(pa) ((((uint64_t)pa) >> 12) << 10)
#define PGROUNDDOWN(a) (((a)) & ~(0xFFF))
#define PGROUNDUP(sz) (((sz) + PG_4K_SIZE - 1) & ~(PG_4K_SIZE - 1))

typedef uint64_t pte;
typedef uint64_t *page_table;
extern page_table kernel_pt;
extern uint8_t vm_init_status;
extern spinlock_t vm_init_lock;

pte *pte_walk(page_table pt, vir_addr_t va, int create);
int kvminit(page_table pt, vir_addr_t va, phys_addr_t pa, uint64_t pages, uint32_t flags);
void init_kvmmap();
void init_kvmhart();
void kvm_do_mapping(page_table pgtable);
#endif