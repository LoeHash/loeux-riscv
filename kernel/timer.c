#include <timer.h>
#include <spinlock.h>
#include <panic.h>
#include <printk.h>

uint8_t timer_inited = 0;
uint64_t sys_timer_tick = 0;
spinlock_t timer_lock = {0};

void init_timer()
{

        // 初始化锁
        init_spinlock(&timer_lock);
        sys_timer_tick = 0;
        __atomic_store_n(&timer_inited, 1, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST); // 可见
}

void do_timer_tick()
{

        if (!timer_inited)
        {
                panic(PANIC_ERROR, "do_timer_tick: has not been inited!\n");
        }

        acquire(&timer_lock);
        sys_timer_tick++;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        // printk("the time tick: %lu\n", sys_timer_tick);
        release(&timer_lock);
}

uint64_t get_sys_timer_tick()
{
        return sys_timer_tick;
}