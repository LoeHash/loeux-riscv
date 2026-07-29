#include "vm.h"
#include "memory.h"
#include "../include/stdint.h"
#include "../include/stddef.h"
#include "../kernel/panic.h"

/// @brief 将 va起始和pa起始构建对应的页表
/// @param pt           根页表
/// @param va           虚拟地址起始
/// @param pa           物理地址起始
/// @param pages        映射的页数
/// @return
int kvminit(page_table pt, vir_addr_t va, phys_addr_t pa, uint64_t pages, uint32_t flags)
{
        // 所有地址，全部向下4kb对其
        vir_addr_t vad = PGROUNDDOWN(va);
        phys_addr_t pad = PGROUNDDOWN(pa);
        pte *p;
        int count = 0;

        while (count < pages)
        {
                // pte_walk
                // 如果遇到没有分配的就会创建
                p = pte_walk(pt, va, 1);
                if (!p)
                {
                        panic(PANIC_ERROR, "kvminit: out of memory!\n");
                }

                // 拿到了这个pte后
                // 我们需要将这个物理地址写入
                // 同时放置flags
                *p = PA2PTE(pad) | flags;
                // 而后，vad和pad全部线性增长4kb
                vad += PG_4K_SIZE;
                pad += PG_4K_SIZE;
                count++;
        }
        return count;
}

/// @brief 根据va，获取对应的l0 pte
/// @param pt
/// @param va
/// @return 返回pte的地址，如果不存在则返回null
/// pte的本质就是
///     如果是l2 l1级别的pte, 那么其实他就是一个地址
///     指向了下一个pte, 否则就是一个普通的pte
///     不管是普通的pte还是其他pte, 他们都是ppn
///     这个ppn是4kb对齐的
pte *pte_walk(page_table pt, vir_addr_t va, int create)
{
        if (va > MAX_VA)
        {
                return NULL;
        }
        pte *p;
        for (int level = 2; level > 0; level--)
        {
                p = &pt[PX(level, va)];
                if (*p & PTE_V)
                {
                        // 已分配的PTE
                        pt = (uint64_t *)PTE2PA(*p);
                }
                else
                {
                        if (!create || (pt = alloc_page()) == NULL)
                        {
                                return 0;
                        }

                        // 如果分配成功
                        // 将这个pt转为pte
                        *p = PA2PTE(pt) | PTE_V;
                }
        }
        return &(pt[PX(0, va)]);
}
