#include "sbi.h"
#include "printk.h"

// ft_addr
// 是fdt规范
// dtb是遵循了这个fdt的规范, 所以对应的, ft_addr就是我们想要的扁平设备树
void kstart(unsigned long hart_id, unsigned long ft_addr)
{

        printk("hello world! this is: %lu\n", hart_id);
        while (1)
                ;
}