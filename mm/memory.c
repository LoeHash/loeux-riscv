#include "memory.h"
#include "vm.h"
#include "../kernel/printk.h"
#include "../kernel/fdt.h"
#include "../include/lib.h"

struct memory_info mem_info = {0};
struct gloal_memory_descriptor gmd = {0};
uint64_t MEMORY_SIZE = 0;

static const uint32_t mem_buff_size = 128;
extern char _phy_start[];
static void print_memory_info();
static void test_page_alloc_free();
static void print_fdt_list();

// fdt 内存节点必须包含 device_type = "memory"
// 如果 address_cells == 1，则 base 是 32 位；
// 如果 == 2，则 base 是 64 位，需从两个 32 位字拼接。
// 同理，size_cells 决定长度是 32 位还是 64 位。
int detect_memory_info(const char *name, int depth,
                       void *node_ptr,
                       void *data)
{
        memset(data, 0, mem_buff_size);

        int res = read_node_prop(node_ptr, "device_type", data, mem_buff_size);

        if (res <= 0)
        {
                return 0;
        }

        if (strcmp(data, "memory") != 0)
        {
                return 0;
        }

        memset(data, 0, mem_buff_size);
        res = read_node_prop(node_ptr, "reg", data, mem_buff_size);
        if (res <= 0)
        {
                return 0;
        }

        // 解析：baseaddress + size
        printk("the reg length: %d\n", res);
        printk("THE BASE MEMORY: %#x\n", bte64(data));
        printk("THE MEMORY SIZE: %lu\n", bte64(data + 8));

        mem_info.banks[mem_info.nr_banks].base = bte64(data);
        mem_info.banks[mem_info.nr_banks].size = bte64(data + 8);
        mem_info.nr_banks++;
        return 0;
}

void init_memory()
{
        char mem_buff[mem_buff_size];
        fdt_walk_nodes((uint64_t)sub_node_base_addr, detect_memory_info, mem_buff);
        // 接下来对于内存进行分配
        // 1. 计算出内存大小
        for (uint32_t i = 0; i < mem_info.nr_banks; i++)
        {

                MEMORY_SIZE += mem_info.banks[i].size;
        }
        printk("init_memory: totoal memory banks: %d\n", mem_info.nr_banks);

        struct page *pg = (struct page *)_phy_start, *pre_pg;
        printk("init_memory: the pg start: %0#x\n", pg);

        struct bank *b;
        char *start, *end;

        for (size_t i = 0; i < mem_info.nr_banks; i++)
        {
                b = &mem_info.banks[i];

                // 接下来，根据这个内存的起始地址以及size
                // 去进行分配
                start = (char *)b->base;
                end = start + b->size;

                // 4kb对齐
                start = (char *)ALIGN_UP((uintptr_t)start, PG_4K_SIZE);
                while (start < end)
                {

                        // 思路: 从现在的对齐内存起始地址开始
                        //      构建出对应的page
                        // 先清0
                        memset(pg, 0, sizeof(struct page));

                        pg->paddr = (phys_addr_t)start;
                        pg->next = 0;
                        pg->prev = gmd.kernel_tail;

                        if (gmd.kernel_tail)
                        {
                                gmd.kernel_tail->next = pg;
                        }
                        else
                        {
                                gmd.kernel_head = pg;
                        }

                        gmd.kernel_tail = pg;
                        gmd.page_length++;
                        pg++;
                        start += PG_4K_SIZE;
                }
        }

        // 接下来，进行特殊处理
        // 因为有一些物理内存是opensbi和kernel本身 + page数据所占用的
        // 所以最终的page结束的位置是在: pg 现在的位置
        start = (char *)MEMORY_START;
        end = (char *)pg;

        while (start < end)
        {
                pg = PHY_TO_PAGE(start);
                pg->flags |= PG_FLAG_RESERVED;
                start += PG_4K_SIZE;
        }

        gmd.free_tail = gmd.kernel_tail; // 初始化空闲节点
        gmd.kernel_tail = pg;            // 保留节点的尾
        gmd.kernel_tail->next = NULL;    // 将保留节点的尾部断开

        pg = PHY_TO_PAGE(start);
        printk("Last not reserved page: addr=%0#x, refcount=%d, flags=%0#x, prev=%0#x, next=%0#x\n",
               pg->paddr,
               pg->refcount,
               pg->flags,
               pg->prev,
               pg->next);

        // 记录第一个，没有被reserved的pg
        gmd.free_head = pg;

        // 将空闲节点的prev断开
        gmd.free_head->prev = NULL;
        gmd.free_tail->next = NULL;

        // 接下来，将fdt设备树的page单独从free的位置拿出来
        // 将设备树地址取出
        pg = PHY_TO_PAGE(ft_base_addr);
        struct page *tmp = PHY_TO_PAGE(PGROUNDDOWN(ft_base_addr + fh_struct.totalsize));
        // 直接设置
        gmd.fdt_head = pg;
        gmd.fdt_tail = tmp;
        // 然后链接
        if (pg->prev) // 绝对是true,不做检查
        {
                pg->prev->next = tmp->next;
        }
        if (tmp->next) // 绝对是true,不做检查
        {
                pg->next->prev = pg->prev;
        }
        // 然后断开链接
        pg->prev = NULL;
        tmp->next = NULL;

        // 设置所有设备树页的保留标志
        for (uintptr_t addr = PGROUNDDOWN(ft_base_addr);
             addr < PGROUNDUP(ft_base_addr + fh_struct.totalsize);
             addr += PG_4K_SIZE)
        {
                struct page *p = PHY_TO_PAGE(addr);
                p->flags |= PG_FLAG_RESERVED;
        }

        printk("After disconnect:\n");
        printk("Free head: paddr=%#x, flags=%#x, prev=%p, next=%p\n",
               gmd.free_head->paddr, gmd.free_head->flags,
               gmd.free_head->prev, gmd.free_head->next);
        printk("Free tail: paddr=%#x, flags=%#x, prev=%p, next=%p\n",
               gmd.free_tail->paddr, gmd.free_tail->flags,
               gmd.free_tail->prev, gmd.free_tail->next);
        test_page_alloc_free();
        print_fdt_list();
        printk("Successfully inited the memory! :)\n");
}

void *alloc_page()
{

        if (!gmd.free_head)
        {
                return NULL;
        }
        struct page *pg = gmd.free_head;
        pg->flags |= PG_FLAG_USED;

        // 我们保证是头部
        // 我们把双向节点的node当作单向节点！
        gmd.free_head = pg->next;
        pg->next = NULL;
        return (void *)pg->paddr;
}

int free_page(void *pa)
{
        struct page *pg = PHY_TO_PAGE(pa);
        memset((char *)pg->paddr, 0, PG_4K_SIZE);
        pg->flags &= 0;
        pg->flags |= PG_FLAG_FREE;
        if (gmd.free_tail)
        {
                gmd.free_tail->next = pg;
        }
        else
        {
                gmd.free_tail = pg;
        }

        return 0;
}

static void print_memory_info()
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

static void test_page_alloc_free()
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

static void print_fdt_list()
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