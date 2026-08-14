#include <vm.h>
#include <memory.h>
#include <stdint.h>
#include <stddef.h>
#include <lib.h>
#include <panic.h>
#include <printk.h>
#include <riscv.h>
#include <spinlock.h>

page_table kernel_pt = 0;
uint8_t vm_init_status = 0;
spinlock_t vm_init_lock = {0};

extern char *_trampoline_jump[];
// turn on the paging
void init_kvmhart()
{
        sfence_vma();
        w_satp(MAKE_SATP(kernel_pt));
        sfence_vma();
}

void init_kvmmap()
{

        acquire(&vm_init_lock);
        if (vm_init_status == 1)
        {
                release(&vm_init_lock);
                return;
        }

        kernel_pt = (page_table)alloc_page();
        memset(kernel_pt, 0, 4096);

        printk("before mapping: %0#lx\n", (uint64_t)kernel_pt);
        kvm_do_mapping(kernel_pt);
        printk("after mapping: %0#lx\n", (uint64_t)kernel_pt);
        // vmprint(kernel_pt);
        pte *ptep = pte_walk(kernel_pt, MMIO_VIRTIO_OFFEST, 0);
        if (ptep == NULL)
        {
                printk("PTE WALK FAILED\n");
        }
        else
        {
                printk("PTE = %lx\n", *ptep);
                printk("PA = %lx\n", PTE2PA(*ptep));
        }
        printk("Finished the kernel mapping. \n");
        printk("opening the MMU...\n");
        init_kvmhart();
        release(&vm_init_lock);
        printk("Done the MMU!\n");
}

int mappages(page_table pagetable, uint64_t va, uint64_t size, uint64_t pa, int perm)
{
        uint64_t a, last;
        pte *pte;

        if ((va % PG_4K_SIZE) != 0)
                panic(PANIC_ERROR, "mappages: va not aligned\n");

        if ((size % PG_4K_SIZE) != 0)
                panic(PANIC_ERROR, "mappages: size not aligned\n");

        if (size == 0)
                panic(PANIC_ERROR, "mappages: size\n");

        a = va;
        last = va + size - PG_4K_SIZE;
        for (;;)
        {
                if ((pte = pte_walk(pagetable, a, 1)) == 0)
                        return -1;
                if (*pte & PTE_V)
                        panic(PANIC_ERROR, "mappages: remap\n");
                *pte = PA2PTE(pa) | perm | PTE_V;
                if (a == last)
                        break;
                a += PG_4K_SIZE;
                pa += PG_4K_SIZE;
        }
        return 0;
}

void kvm_do_mapping(page_table pgtable)
{

        kvminit(pgtable,
                KERNEL_START,
                KERNEL_START,
                ((gmd.kernel_tail->paddr - KERNEL_START) / PG_4K_SIZE) + 1,
                PTE_V | PTE_R | PTE_W | PTE_X, 0);

        // 2. 映射设备树 (FDT)
        kvminit(pgtable,
                gmd.fdt_head->paddr,
                gmd.fdt_head->paddr,
                ((gmd.fdt_tail->paddr - gmd.fdt_head->paddr) / PG_4K_SIZE) + 1,
                PTE_V | PTE_R, 0);

        // 映射空闲内存
        kvminit(pgtable,
                gmd.free_start_at,
                gmd.free_start_at,
                ((gmd.free_end_at - gmd.free_start_at) / PG_4K_SIZE) + 1,
                PTE_V | PTE_R | PTE_W, 0);

        // // 映射 UART MMIO
        // kvminit(pgtable,
        //         MMIO_UART_OFFEST,
        //         UART_BASE,
        //         UART_PAGE_SIZE,
        //         PTE_V | PTE_R | PTE_W, 1);

        // // 映射 VirtIO MMIO
        // kvminit(pgtable,
        //         MMIO_VIRTIO_OFFEST,
        //         VIRTIO_MMIO_BASE,
        //         VIRTIO_PAGE_SIZE,
        //         PTE_V | PTE_R | PTE_W, 1);

        // // 映射 CLINT MMIO
        // kvminit(pgtable,
        //         MMIO_CLINT_OFFEST,
        //         CLINT_BASE,
        //         CLINT_PAGE_SIZE,
        //         PTE_V | PTE_R | PTE_W, 1);

        // // 映射 PLIC MMIO
        // kvminit(pgtable,
        //         MMIO_PLIC_OFFEST,
        //         PLIC_BASE,
        //         PLIC_PAGE_SIZE,
        //         PTE_V | PTE_R | PTE_W | PTE_X, 1);

        // // 映射 trampline
        // kvminit(pgtable,
        //         TRAMPOLINE,
        //         (uint64_t)_trampoline_jump,
        //         1,
        //         PTE_V | PTE_R | PTE_U | PTE_X, 1);

        // 映射 UART MMIO
        kvminit(pgtable,
                MMIO_UART_OFFEST,
                UART_BASE,
                UART_PAGE_SIZE,
                PTE_V | PTE_R | PTE_W | PTE_U, 0);

        // 映射 VirtIO MMIO
        kvminit(pgtable,
                MMIO_VIRTIO_OFFEST,
                VIRTIO_MMIO_BASE,
                VIRTIO_PAGE_SIZE,
                PTE_V | PTE_R | PTE_W, 0);

        // 映射 CLINT MMIO
        kvminit(pgtable,
                MMIO_CLINT_OFFEST,
                CLINT_BASE,
                CLINT_PAGE_SIZE,
                PTE_V | PTE_R | PTE_W, 0);

        // 映射 PLIC MMIO
        kvminit(pgtable,
                MMIO_PLIC_OFFEST,
                PLIC_BASE,
                PLIC_PAGE_SIZE,
                PTE_V | PTE_R | PTE_W | PTE_X, 0);

        // 映射 trampline
        kvminit(pgtable,
                TRAMPOLINE,
                (uint64_t)_trampoline_jump,
                1,
                PTE_V | PTE_R | PTE_U | PTE_X, 0);
}

/// @brief 将 va起始和pa起始构建对应的页表
/// @param pt           根页表
/// @param va           虚拟地址起始
/// @param pa           物理地址起始
/// @param pages        映射的页数
/// @return count 映射的页数 0 错误
int kvminit(page_table pt, vir_addr_t va, phys_addr_t pa, uint64_t pages, uint32_t flags, int debug)
{
        // 所有地址，全部向下4kb对其
        vir_addr_t vad = PGROUNDDOWN(va);
        phys_addr_t pad = PGROUNDDOWN(pa);
        pte *p;
        int count = 0;

        while (count < pages)
        {
                if (debug == 1)
                {
                        // // 打印参数
                        // printk("[kvminit] va = %0#lx, pa = %#x, pages = %lu, flags = %#x\n",
                        //        vad, pad, pages, flags);
                }
                // pte_walk
                // 如果遇到没有分配的就会创建
                p = pte_walk(pt, vad, 1);
                if (!p)
                {
                        panic(PANIC_ERROR, "kvminit: out of memory!\n");
                }

                // 拿到了这个pte后
                // 我们需要将这个物理地址写入
                // 同时放置flags
                *p = PA2PTE(pad) | flags;

                if (debug == 1)
                {
                        printk("va: %0#lx, pte2pa: %0#lx\n", vad, PTE2PA(*p));
                }

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
                                printk("really out!\n");
                                return 0;
                        }

                        // 如果分配成功
                        // 将这个pt转为pte
                        *p = PA2PTE(pt) | PTE_V;
                }
        }
        return &(pt[PX(0, va)]);
}

/// @brief 创建一个新的页表
/// @return
page_table pg_create()
{
        page_table pg;

        if ((pg = (page_table)kalloc()) == 0)
        {
                return 0;
        }

        memset(pg, 0, PG_4K_SIZE);

        return pg;
}

void pg_unmap(page_table pagetable, uint64_t va, uint64_t npages, int do_free)
{
        uint64_t a;
        pte *pte;

        if ((va % PG_4K_SIZE) != 0)
                panic(PANIC_ERROR, "pg_unmap: not aligned");

        for (a = va; a < va + npages * PG_4K_SIZE; a += PG_4K_SIZE)
        {
                if ((pte = pte_walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
                        continue;
                if ((*pte & PTE_V) == 0) // has physical page been allocated?
                        continue;
                if (do_free)
                {
                        uint64_t pa = PTE2PA(*pte);
                        kfree((void *)pa);
                }
                *pte = 0;
        }
}

// 取消所有的映射
// 同时释放页表的所有内存
// 隐性的限制条件是: 用户代码在 0x0 处加载
void pg_user_vmfree(page_table pagetable, uint64_t sz)
{
        if (sz > 0)
                pg_unmap(pagetable, 0, PGROUNDUP(sz) / PG_4K_SHIFT, 1);
        freewalk(pagetable);
}

/// @brief 回收pg中所有分配的物理页
/// @param pagetable
void freewalk(page_table pagetable)
{
        // there are 2^9 = 512 PTEs in a page table.
        for (int i = 0; i < 512; i++)
        {
                pte pte = pagetable[i];
                if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
                {
                        // this PTE points to a lower-level page table.
                        uint64_t child = PTE2PA(pte);
                        freewalk((page_table)child);
                        pagetable[i] = 0;
                }
                else if (pte & PTE_V)
                {
                        panic(PANIC_ERROR, "freewalk: leaf\n");
                }
        }
        kfree((void *)pagetable);
}