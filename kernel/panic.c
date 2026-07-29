#include "panic.h"
#include "printk.h"
#include "../include/stdarg.h"

void print_notice_flag(int panic_id)
{
        printk("[");
        switch (panic_id)
        {
        case PANIC_ERROR: // error
                printk("ERROR");
                break;
        case PANIC_WRONG: // wrong
                printk("WRONG");
                break;
        default:
                printk("!");
                break;
        }
        printk("] ");
}

void panic(int panic_id, char *reason, ...)
{
        print_notice_flag(panic_id);

        va_list args;
        char buf[PRINT_BUFFER_SIZE];
        va_start(args, reason);
        vsprintf(buf, reason, args); // 将 va_list 传递给 vsprintf
        va_end(args);
        printk("%s", buf);

        if (panic_id == 1)
        {
                while (1)
                        ;
        }
}