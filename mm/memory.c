#include "memory.h"
#include "../kernel/printk.h"
#include "../kernel/fdt.h"
#include "../include/lib.h"

struct memory_info mem_info = {0};
struct gloal_memory_descriptor gmd = {0};
uint64_t MEMORY_SIZE = 0;
static const uint32_t mem_buff_size = 128;
extern char _phy_start[];

static void print_memory_info();

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
                        pg->prev = gmd.tail;

                        if (gmd.tail)
                        {
                                gmd.tail->next = pg;
                        }
                        else
                        {
                                gmd.head = pg;
                        }

                        gmd.tail = pg;
                        gmd.page_length++;
                        pg++;
                        start += PG_4K_SIZE;
                }
        }

        // 接下来，进行特殊处理
        // 因为有一些物理内存是opensbi和kernel本身 + page数据所占用的
        // 所以最终的page结束的位置是在:
        start = (char *)MEMORY_START;
        end = (char *)pg;

        while (start < end)
        {
                pg = PHY_TO_PAGE(start);
                pg->flags |= PG_FLAG_RESERVED;
                start += PG_4K_SIZE;
        }
        printk("memory has been inited!\n");
        print_memory_info();
}

static void print_memory_info()
{
        printk("=== Global Memory Descriptor ===\n");
        printk("page_length: %lu\n", gmd.page_length);
        printk("head: %0#x\n", gmd.head);
        printk("tail: %0#x\n", gmd.tail);
        printk("pg: %0#x\n", gmd.pg);

        printk("\n=== Page List ===\n");
        struct page *cur = gmd.head;
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