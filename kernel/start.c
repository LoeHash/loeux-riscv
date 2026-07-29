#include "sbi.h"
#include "fdt.h"
#include "printk.h"
#include "riscv.h"
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
void init_fdt(unsigned long ft_addr)
{
        ft_base_addr = ft_addr;
        printk("ft_base_addr = 0x%lx\n", ft_base_addr);
        fdt_header_init();
        printk("fdt header and root node has been inited :) \n");
        fdt_walk_nodes((uint64_t)sub_node_base_addr, walk_fdt, 0);
        printk("Successfully Detected the fdt infomation! :)\n");
}
void kstart(unsigned long hart_id, unsigned long ft_addr)
{

        // fdt solve.
        init_fdt(ft_addr);
        printk("the fdt size = %ld\n", fh_struct.totalsize);
        printk("Successfully inited the fdt! :)\n");

        init_memory();
        printk("Successfully inited the memory! :)\n");

        uint64_t satp_val = r_satp();
        printk("satp = %lx, MODE = %ld\n", satp_val, (satp_val >> 60) & 0xf);

        while (1)
                ;
}