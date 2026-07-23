#include "sbi.h"
#include "printk.h"

void kstart(unsigned long hart_id, unsigned long ft_addr)
{
        printk("hello world! this is: %lu\n", hart_id);
        while (1)
                ;
}