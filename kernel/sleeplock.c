#include <spinlock.h>
#include <type.h>
#include <sleeplock.h>
#include <memory.h>
#include <panic.h>
static void _release_sleep_proto(struct sleeplock *slk, int is_all);
static void wakeup(struct sleeplock *slk);
static void wakeup_all(struct sleeplock *slk);
/*
 * 睡眠锁实现
 * @note 睡眠锁是一种基于等待队列的锁，用于保护共享资源的访问。
 *       当一个进程获取到睡眠锁后，其他进程会进入睡眠状态，等待该进程释放锁。
 *       释放锁后，等待队列里的进程会被唤醒，重新进入运行状态。
 *       睡眠锁的实现基于等待队列，每个进程都有一个等待节点，用于存储该进程的等待状态。
 *       等待节点通过双向链表连接起来，形成一个等待队列。
 *       等待队列的头尾节点指向哨兵节点，用于方便操作。
 * @note 此锁是一个不可重入锁
 **/



/// @brief 调用者必须持有slk->spinlock
///        把等待队列里的所有睡眠进程摘下来，改成 RUNNABLE
/// @param slk 睡眠锁
/// @return 无
/// @note 该函数会修改睡眠锁的等待队列，所有进程的等待状态会被设置为 RUNNABLE
///       此函数为内部函数，仅供 _release_sleep_proto 在持锁状态下调用，
///       不对外暴露，避免外部调用者在不持锁时操作等待队列引发数据竞争。
static void wakeup_all(struct sleeplock *slk)
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
/// @param slk 睡眠锁
/// @return 无
/// @note 该函数会修改睡眠锁的等待队列，将一个进程的等待状态设置为 RUNNABLE
///       此函数为内部函数，仅供 _release_sleep_proto 在持锁状态下调用，
///       不对外暴露，避免外部调用者在不持锁时操作等待队列引发数据竞争。
static void wakeup(struct sleeplock *slk)
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

/// @brief 初始化睡眠锁
/// @param slk 睡眠锁
/// @return 无
/// @note 该函数会初始化睡眠锁的等待队列，将所有进程的等待状态设置为 SLEEP
void init_sleeplock(struct sleeplock *slk)
{
        slk->locked = 0;
        slk->holder = 0;
        init_spinlock(&slk->spinlock);
        slk->wait_queue.next = &slk->wait_queue;
        slk->wait_queue.prev = &slk->wait_queue;
        slk->wait_queue.task = 0;
}

/// @brief 获取睡眠锁
///        若没有获取, 则进入睡眠
///        由其他进程wakeup
/// @param slk 睡眠锁
/// @return 无
/// @note 该函数会修改睡眠锁的等待队列，将当前进程的等待状态设置为 SLEEP
void acquire_sleep(struct sleeplock *slk)
{
        struct task_struct *ts = get_task();

        // 先获取内部spinlock
        acquire(&slk->spinlock);

        if(slk->holder == ts->pid)
        {
                panic(PANIC_ERROR, "acquire_sleep: Already a Owner! pid: %d\n", ts->pid);
        }

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

/// @brief 释放睡眠锁
/// @param slk 睡眠锁
/// @return 无
/// @note 该函数会将当前进程的等待状态设置为 RUNNABLE
void release_sleep(struct sleeplock *slk)
{
        _release_sleep_proto(slk, 0);
}

/// @brief 释放睡眠锁
/// @param slk 睡眠锁
/// @return 无
/// @note 该函数会将所有进程的等待状态设置为 RUNNABLE
void release_sleep_all(struct sleeplock *slk)
{
        _release_sleep_proto(slk, 1);
}

/// @brief 释放睡眠锁
/// @param slk 睡眠锁
/// @param is_all 是否释放所有进程的睡眠锁
/// @return 无
/// @note 该函数会将当前进程的等待状态设置为 RUNNABLE，或唤醒所有进程
static void _release_sleep_proto(struct sleeplock *slk, int is_all)
{
        struct task_struct *ts = get_task();
        // 必须先获取 spinlock 再读 holder：
        // holder 由 acquire_sleep 在临界区内写入，
        // 无锁读取可能拿到撕裂/陈旧值，导致误 panic 或漏判。
        acquire(&slk->spinlock);
        if (slk->holder != ts->pid)
        {
                release(&slk->spinlock);
                panic(PANIC_ERROR, "release_sleep: Not a Owner! pid: %d\n", ts->pid);
        }
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