#ifndef _INC_TIMER
#define _INC_TIMER
#include "type.h"

/*
 * QEMU virt 的 mtime 频率为 10MHz 也就是说是 每秒 0x989680 个 cycle
 * 时钟节拍 = BASE_FREQUENCY / TIMER_TICKS_PER_SEC。
 *
 * 注意：必须是 100Hz
 * 之前误用 TASK_CPU_SLIP_FACTOR=10 作除数，节拍变成 100ms（10Hz），
 * 导致依赖 wfi+时钟轮询的键盘最坏 100ms 才响应，打字明显发肉；
 * ext2/fat32 的时间戳也按 换算秒，即以 100Hz 为准。
 */
#define BASE_FREQUENCY 0x989680
#define TIMER_TICKS_PER_SEC 100
#define TASK_CPU_SLIP_FACTOR 10

extern uint64_t sys_timer_tick;
extern spinlock_t timer_lock;
extern uint8_t timer_inited;

void do_timer_tick();
void init_timer();
uint64_t get_sys_timer_tick();
#endif