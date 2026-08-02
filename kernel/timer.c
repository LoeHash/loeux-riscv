#include <timer.h>
#include <spinlock.h>
#include <panic.h>

uint8_t timer_inited = 0;
uint64_t sys_timer_tick = 0;
spinlock_t timer_lock = {0};

void init_timer()
{
}

void do_timer_tick()
{

        if (!timer_inited)
        {
                panic(PANIC_ERROR, "do_timer_tick: has not been inited!\n");
        }

        acquire(&timer_lock);
        sys_timer_tick++;
        release(&timer_lock);
}

uint64_t get_sys_timer_tick()
{
        return sys_timer_tick;
}