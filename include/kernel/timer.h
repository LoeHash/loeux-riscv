#ifndef _INC_TIMER
#define _INC_TIMER
#include "type.h"
#define BASE_FREQUENCY 0x989680
#define TASK_CPU_SLIP_FACTOR 10

extern uint64_t sys_timer_tick;
extern spinlock_t timer_lock;
extern uint8_t timer_inited;

void do_timer_tick();
void init_timer();
uint64_t get_sys_timer_tick();
#endif