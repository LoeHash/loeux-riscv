#include <timer.h>
#include <spinlock.h>
#include <panic.h>
#include <printk.h>
#include <drivers/gpu/kgfx.h>

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

        /*
         * 时钟驱动的屏幕刷新：把这一拍内累积的脏区合并成一次
         * GPU 传输 放锁外执行，慢 IO 不占 timer_lock。
         * kgfx 内部自带锁且 attach 前直接返回。
         */
        kgfx_timer_tick();
}

uint64_t get_sys_timer_tick()
{
        return sys_timer_tick;
}