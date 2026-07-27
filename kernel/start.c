#include "sbi.h"
#include "fdt.h"
#include "printk.h"

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

static void init_fdt(unsigned long ft_addr)
{
        ft_base_addr = ft_addr;
        printk("ft_base_addr = 0x%lx\n", ft_base_addr);
        fdt_header_init();
        printk("fdt header and root node has been inited :) \n");
        fdt_walk_nodes((uint64_t)sub_node_base_addr, walk_fdt, 0);
        printk("Successfully Detecting the fdt infomation! :)");
}

// ft_addr
// 是fdt规范
// dtb是遵循了这个fdt的规范, 所以对应的, ft_addr就是我们想要的扁平设备树
void kstart(unsigned long hart_id, unsigned long ft_addr)
{

        // fdt solve.
        init_fdt(ft_addr);

        while (1)
                ;
}