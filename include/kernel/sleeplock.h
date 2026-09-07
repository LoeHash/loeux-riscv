#ifndef _INC_SLEEPLOCK_
#define _INC_SLEEPLOCK_
#include <type.h>

// sleep锁语义:
// 拿不到锁时，让出 CPU，进入睡眠，等锁被释放时被唤醒。
//
// === 锁序约定 (LOCK ORDERING) ===
// 当同时持有 sleeplock 的内部 spinlock 与 task_struct::lk 时，
// 必须按以下顺序获取，否则可能引发 AB-BA 死锁：
//
//      1. slk->spinlock      （sleeplock 内部自旋锁）
//      2. ts->lk             （任务结构体自旋锁）
//
// 涉及该序的代码路径：
//   - acquire_sleep()：先 acquire(slk->spinlock)，循环内 acquire(ts->lk)
//   - _release_sleep_proto()：先 acquire(slk->spinlock)，wakeup()/wakeup_all()
//     内部再 acquire(ts->lk)
//
// 反向获取（先 ts->lk 再 slk->spinlock）在任何地方都禁止出现。
// 如需在持 ts->lk 时操作 sleeplock，请重构为分阶段获取。

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

#endif