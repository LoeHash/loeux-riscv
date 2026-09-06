#include <spinlock.h>
#include <type.h>
#include <sleeplock.h>
#include <memory.h>
#include <panic.h>
static void _release_sleep_proto(struct sleeplock *slk, int is_all);

// 把等待队列里的所有睡眠进程摘下来，改成 RUNNABLE
void wakeup_all(struct sleeplock *slk)
{
        struct wait_node *pos;
        struct wait_node *nxt; // 用于保存下一个节点

        for (pos = slk->wait_queue.next; pos != &slk->wait_queue; pos = nxt)
        {
                nxt = pos->next;

                // 摘除当前节点 pos
                pos->prev->next = pos->next;
                pos->next->prev = pos->prev;

                // 唤醒任务
                struct task_struct *ts = pos->task;
                acquire(&ts->lk);
                ts->state = RUNNABLE;
                release(&ts->lk);
        }

        // 重置哨兵
        slk->wait_queue.next = &slk->wait_queue;
        slk->wait_queue.prev = &slk->wait_queue;
}

/// @brief 调用者必须持有slk->spinlock
///        把等待队列里的一个睡眠进程摘下来，改成 RUNNABLE
/// @param slk
void wakeup(struct sleeplock *slk)
{

        // 找到一个
        if (slk->wait_queue.next != &slk->wait_queue)
        {
                // 摘除
                struct wait_node *alive = slk->wait_queue.next;

                alive->prev->next = alive->next;
                alive->next->prev = alive->prev;

                struct task_struct *ts = alive->task;
                acquire(&ts->lk);
                ts->state = RUNNABLE;
                release(&ts->lk);
        }
}

void init_sleeplock(struct sleeplock *slk)
{
        slk->locked = 0;
        init_spinlock(&slk->spinlock);
        slk->wait_queue.next = &slk->wait_queue;
        slk->wait_queue.prev = &slk->wait_queue;
        slk->wait_queue.task = 0;
}

/// @brief 获取睡眠锁
///        若没有获取, 则进入睡眠
///        由其他进程wakeup
/// @param slk
void acquire_sleep(struct sleeplock *slk)
{
        struct task_struct *ts = get_task();

        // 先获取内部spinlock
        acquire(&slk->spinlock);

        // 如果已经锁上了
        while (slk->locked)
        {
                acquire(&ts->lk);
                // 则更改进程状态
                ts->state = SLEEP;
                // 将此task加入到队列
                ts->sleep_node.task = ts;
                struct wait_node *tail = slk->wait_queue.prev;

                tail->next = &ts->sleep_node;
                ts->sleep_node.prev = tail;
                ts->sleep_node.next = &slk->wait_queue; // next 指向哨兵
                slk->wait_queue.prev = &ts->sleep_node;

                // 同时释放内部spinlock
                release(&slk->spinlock);

                // 这里切出去
                sched();

                // 回来后
                // 当调度完成,重新获取spinlock
                release(&ts->lk);
                acquire(&slk->spinlock);
        }

        // 走到这里说明已经成功上锁
        slk->locked = 1;
        slk->holder = ts->pid;
        MEMORY_FENCE;
        release(&slk->spinlock);
}

void release_sleep(struct sleeplock *slk)
{
        _release_sleep_proto(slk, 0);
}

void release_sleep_all(struct sleeplock *slk)
{
        _release_sleep_proto(slk, 1);
}

static void _release_sleep_proto(struct sleeplock *slk, int is_all)
{
        struct task_struct *ts = get_task();
        if (slk->holder != ts->pid)
        {
                panic(PANIC_ERROR, "release_sleep: Not a Owner! pid: %d\n", ts->pid);
        }
        acquire(&slk->spinlock);
        slk->holder = 0;
        slk->locked = 0;
        if (is_all == 1)
        {
                wakeup_all(slk);
        }
        else
        {
                wakeup(slk);
        }
        release(&slk->spinlock);
}