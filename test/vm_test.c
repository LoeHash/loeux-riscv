#include <vm.h>
#include <type.h>
#include <test.h>
#include <printk.h>
static int is_valid(pte_t p);

static uint64_t sv39_idx_to_va(uint64_t l2, uint64_t l1, uint64_t l0);

static int is_valid(pte_t p)
{
        return (p & PTE_V) && (p & (PTE_R | PTE_W | PTE_X)) == 0;
}

void vmprint(page_table pgtb)
{
        printk("page table %p\n", pgtb);

        for (int i = 0; i < 512; i++)
        {
                // 获取当前的叶子节点
                pte_t pi = pgtb[i];
                // 所有的非叶子节点都是v为1, 且 rwx为0
                if (!is_valid(pi))
                {
                        continue;
                }
                page_table ichild = ((page_table)PTE2PA(pi));
                printk(" ..%d: pte %p\t\tpa: %p\n", i, (uint64_t *)pi, (uint64_t *)ichild);

                for (int j = 0; j < 512; j++)
                {

                        pte_t pj = ichild[j];
                        if (!is_valid(pj))
                        {
                                continue;
                        }

                        page_table jchild = ((page_table)PTE2PA(pj));
                        printk(" ..");
                        printk(" ..%d: pte %p\tpa: %p\n", j, (uint64_t *)pj, (uint64_t *)jchild);

                        for (int k = 0; k < 512; k++)
                        {
                                pte_t pk = jchild[k];
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

/**
 * vm_compare_full_pagetable
 * 三层for手动遍历SV39完整页表树，对比两个页表全部有效叶子页内容
 * 不返回对错，发现页内容不一致直接打印va、pa1、pa2，继续遍历全部
 * @param pt1 页表1
 * @param pt2 页表2
 */
int vm_compare_full_pagetable(page_table pt1, page_table pt2)
{
        pte *p1, *p2;
        uint64_t va, pa, pa2;
        char *chunk;
        int32_t flag;

        va = 0;
        for (; va <= 0xFFFFFFFFFFFFFFF; va += PG_4K_SIZE)
        {
                if ((p1 = pte_walk(pt1, va, 0)) == 0 || (p2 = pte_walk(pt2, va, 0)) == 0)
                {
                        continue;
                }

                if ((*p1 & PTE_V) == 0 || (*p2 & PTE_V) == 0)
                {
                        continue;
                }
                pa = PTE2PA(*p1);
                pa2 = PTE2PA(*p2);
                if (pa == 0 || pa2 == 0)
                {
                        continue;
                }

                pa = PTE2PA(*p1);
                pa2 = PTE2PA(*p2);
                if (pa == 0 || pa2 == 0)
                {
                        continue;
                }

                if (memcmp((char *)pa, (char *)pa2, PG_4K_SIZE) != 0)
                {
                        printk("vm_compare_full_pagetable: va %p, pa1 %p, pa2 %p\n", (void *)va, (void *)pa, (void *)pa2);
                }
        }

        return 0;
}
