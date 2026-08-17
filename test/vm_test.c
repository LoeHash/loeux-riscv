#include <vm.h>
#include <test.h>
#include <printk.h>
static int is_valid(pte p);

static uint64_t sv39_idx_to_va(uint64_t l2, uint64_t l1, uint64_t l0);

static int is_valid(pte p)
{
        return (p & PTE_V) && (p & (PTE_R | PTE_W | PTE_X)) == 0;
}

void vmprint(page_table pgtb)
{
        printk("page table %p\n", pgtb);

        for (int i = 0; i < 512; i++)
        {
                // 获取当前的叶子节点
                pte pi = pgtb[i];
                // 所有的非叶子节点都是v为1, 且 rwx为0
                if (!is_valid(pi))
                {
                        continue;
                }
                page_table ichild = ((page_table)PTE2PA(pi));
                printk(" ..%d: pte %p\t\tpa: %p\n", i, (uint64_t *)pi, (uint64_t *)ichild);

                for (int j = 0; j < 512; j++)
                {

                        pte pj = ichild[j];
                        if (!is_valid(pj))
                        {
                                continue;
                        }

                        page_table jchild = ((page_table)PTE2PA(pj));
                        printk(" ..");
                        printk(" ..%d: pte %p\tpa: %p\n", j, (uint64_t *)pj, (uint64_t *)jchild);

                        for (int k = 0; k < 512; k++)
                        {
                                pte pk = jchild[k];
                                if ((pk & PTE_V))
                                {
                                        printk(" .. ..");
                                        printk(" ..%d: va %p\tpa: %p\n", k, (uint64_t *)sv39_idx_to_va(i, j, k), (uint64_t *)PTE2PA(pk));
                                }
                        }
                }
        }
}

static uint64_t sv39_idx_to_va(uint64_t l2, uint64_t l1, uint64_t l0)
{

        uint64_t vpn = (l2 << 18) | (l1 << 9) | l0;
        uint64_t va = vpn << 12;

        return va;
}