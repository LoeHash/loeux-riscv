#include "sbi.h"
#include "fdt.h"
#include "printk.h"
#include "riscv.h"
#include "../mm/memory.h"
#include "../mm/vm.h"

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
        // printk("ft_base_addr = 0x%lx\n", ft_base_addr);
        fdt_header_init();
        printk("fdt header and root node has been inited :) \n");
        fdt_walk_nodes((uint64_t)sub_node_base_addr, walk_fdt, 0);
        printk("Successfully Detected the fdt infomation! :)\n");
        printk("the fdt size = %ld\n", fh_struct.totalsize);
        printk("Successfully inited the fdt! :)\n");
}

void kstart(unsigned long hart_id, unsigned long ft_addr)
{

        // fdt solve.
        init_fdt(ft_addr);
        init_memory();
        // 构建内核页表
        init_kvmmap();
        // 启动mmu！
        init_kvmhart();

        // we are in mmu!
        printk("here we are!\n");

        while (1)
                ;
}