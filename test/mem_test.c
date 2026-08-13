#include <memlayout.h>
#include <memory.h>
#include <printk.h>
#include <test.h>

extern char _phy_start[];

void print_memory_info()
{
        printk("=== Global Memory Descriptor ===\n");
        printk("page_length: %lu\n", gmd.page_length);
        printk("free_head: %0#x\n", gmd.free_head);
        printk("free_tail: %0#x\n", gmd.free_tail);
        printk("pg: %0#x\n", gmd.pg);

        printk("\n=== Page List ===\n");
        struct page *cur = gmd.free_head;
        int count = 0;

        while (cur)
        {
                printk("Page[%d]: addr=%0#x, refcount=%d, flags=%0#x, prev=%0#x, next=%0#x\n",
                       count++,
                       cur->paddr,
                       cur->refcount,
                       cur->flags,
                       cur->prev,
                       cur->next);
                cur = cur->next;
        }

        printk("Total pages: %d\n", count);
}

void test_page_alloc_free()
{

        void *p1, *p2;

        p1 = alloc_page();
        printk("alloc 1: p1=%p\n", p1);
        PRINT_PAGE_INFO(p1, "alloc 1");

        p2 = alloc_page();
        printk("alloc 2: p2=%p\n", p2);
        PRINT_PAGE_INFO(p2, "alloc 2");

        free_page(p1);
        printk("freed p1\n");
        PRINT_PAGE_INFO(p1, "after free p1");

        // 此时空闲链表 head 应该指向 p1（因为 p1 被插回了头部）
        // 但全局链表里 p1 的 prev/next 可能已经被 free 过程中的插入打乱
        // 验证：再次 alloc，应该得到 p1 而不是 p2
        void *p3 = alloc_page();
        printk("alloc 3: p3=%p\n", p3);
        PRINT_PAGE_INFO(p3, "alloc 3");

        // 如果 p3 == p1，说明 free 插入头部正确，空闲链表没问题。
        // 但此时遍历全局链表（gmd.head → tail），看顺序是否还连续？
        struct page *pg = gmd.free_head;
        int count = 0;
        while (pg)
        {
                count++;
                if (count > 10)
                        break; // 只打印前几个
                printk("  pg addr=%0#x, flags=%0#x, next=%0#x\n", pg, pg->flags, pg->next);
                pg = pg->next;
        }
}

void print_fdt_list()
{
        if (!gmd.fdt_head)
        {
                printk("fdt list is empty.\n");
                return;
        }

        const struct page *p;
        int count = 0;
        for (p = gmd.fdt_head; p != NULL; p = p->next)
        {
                printk("node %d: "
                       "paddr=%0#x, "
                       "refcount=%0#x, "
                       "flags=%0#x, "
                       "prev=%0#x, "
                       "next=%0#x\n",
                       count,
                       (unsigned int)p->paddr,
                       (unsigned int)p->refcount,
                       (unsigned int)p->flags,
                       (unsigned int)(uintptr_t)p->prev,
                       (unsigned int)(uintptr_t)p->next);
                count++;
        }
        printk("total %d nodes printed.\n", count);
}