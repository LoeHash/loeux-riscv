#include <timer.h>
#include <spinlock.h>
#include <panic.h>
#include <printk.h>
#include <riscv.h>
#include <drivers/tty/gpu_tty.h>
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

	if (!timer_inited) {
		panic(PANIC_ERROR, "do_timer_tick: has not been inited!\n");
	}

	acquire(&timer_lock);
	sys_timer_tick++;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	release(&timer_lock);

	/*
	 * GPU 刷新 + 光标闪烁只在 hart 0 执行，避免 4 核竞争导致
	 * blink_counter 增长 4 倍速
	 * sys_timer_tick 在 timer_lock 内递增，4 核每 tick 各加一次
	 */
	if (r_tp() != 0)
		return;

	gpu_tty_tick();
	kgfx_timer_tick();
}

uint64_t get_sys_timer_tick()
{
	return sys_timer_tick;
}