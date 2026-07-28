#include "sbi.h"
#include "fdt.h"
#include "printk.h"
#include "../mm/memory.h"

unsigned long ft_base_addr;

int walk_fdt(const char *name, int depth,
             void *node_ptr,
             void *data)
{
        // 打印缩进
        for (int i = 0; i < depth; i++)
                printk("  ");

        // 打印节点名称和地址
        printk("%s (0x%p)\n", name, node_ptr);

        return 0;
}

void kstart(unsigned long hart_id, unsigned long ft_addr)
{

        // fdt solve.
        ft_base_addr = ft_addr;
        // printk("ft_base_addr = 0x%lx\n", ft_base_addr);
        fdt_header_init();
        printk("fdt header and root node has been inited :) \n");
        fdt_walk_nodes((uint64_t)sub_node_base_addr, walk_fdt, 0);
        printk("Successfully Detecting the fdt infomation! :)\n");

        init_memory();
        printk("Successfully init for the memory! :)\n");
        while (1)
                ;
}