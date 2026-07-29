#ifndef __KERNEL_PANIC_H_
#define __KERNEL_PANIC_H_

#define PANIC_FRONT_COLOR RED
#define PANIC_BACK_COLOR BLACK

#define PANIC_ERROR 1
#define PANIC_WRONG 2

void panic(int panic_id, char *reason, ...);
void print_notice_flag(int panic_id);
#endif