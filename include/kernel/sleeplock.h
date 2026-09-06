#ifndef _INC_SLEEPLOCK_
#define _INC_SLEEPLOCK_
#include <type.h>

// sleep锁语义:
// 拿不到锁时，让出 CPU，进入睡眠，等锁被释放时被唤醒。

struct sleeplock
{
        spinlock_t spinlock;
        uint8_t locked;
        pid_t holder;
        struct wait_node wait_queue;
};
typedef struct sleeplock sleeplock_t;

void release_sleep(struct sleeplock *slk);
void release_sleep_all(struct sleeplock *slk);
void acquire_sleep(struct sleeplock *slk);
void init_sleeplock(struct sleeplock *slk);
void wakeup(struct sleeplock *slk);
void wakeup_all(struct sleeplock *slk);

#endif