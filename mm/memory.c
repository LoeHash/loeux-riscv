#include <mm/memory.h>
#include <mm/vm.h>
#include <printk.h>
#include <fdt.h>
#include <lib.h>
#include <spinlock.h>

struct memory_info mem_info = {0};
struct gloal_memory_descriptor gmd = {0};
uint64_t MEMORY_SIZE = 0;
uint8_t memory_init_status = 0;
spinlock_t memory_init_lock = {0};

static const uint32_t mem_buff_size = 128;
extern char _phy_start[];

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
        acquire(&memory_init_lock);
        if (memory_init_status == 1)
        {
                printk("bumped into the gap!\n hart id: %d", get_cpu_id());
                release(&memory_init_lock);
                return;
        }

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

        // 现在的free_start 就是绝对意义上的 第一个空闲页
        gmd.free_start_at = gmd.free_head->paddr;
        gmd.free_end_at = gmd.free_tail->paddr;
        // 现在的free_tail 就是绝对意义上的 最后一个空闲页

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
                printk("fdt checking: %0#x\n", p->paddr);
        }

        // printk("After disconnect:\n");
        // printk("Free head: paddr=%#x, flags=%#x, prev=%p, next=%p\n",
        //        gmd.free_head->paddr, gmd.free_head->flags,
        //        gmd.free_head->prev, gmd.free_head->next);
        // printk("Free tail: paddr=%#x, flags=%#x, prev=%p, next=%p\n",
        //        gmd.free_tail->paddr, gmd.free_tail->flags,
        //        gmd.free_tail->prev, gmd.free_tail->next);
        // test_page_alloc_free();
        // print_fdt_list();
        printk("Successfully inited the memory! :)\n");

        memory_init_status = 1;
        release(&memory_init_lock);
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
        memset((char *)pg->paddr, 0, PG_4K_SIZE);

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
