#include <spinlock.h>
#include <proc.h>
#include <riscv.h>
#include <panic.h>

void init_spinlock(spinlock_t *lk)
{
        lk->val = 0;
        lk->holder = 0;
}

// we need atom
void acquire(spinlock_t *lk)
{
        // 先关闭中断
        push_off();

        // 这个spinlock是不可重入锁
        if (is_holding(lk))
        {
                panic(PANIC_ERROR, "acquire: reacquire who? hart: %d\n", get_cpu_id());
        }

        while (__atomic_exchange_n(&lk->val, 1, __ATOMIC_ACQUIRE) != 0)
                ;

        lk->holder = get_cpu();
}

void release(spinlock_t *lk)
{

        // 这个spinlock是不可重入锁
        if (!is_holding(lk))
        {
                panic(PANIC_ERROR, "release: not a owner!\n");
        }

        lk->holder = 0;

        __atomic_store_n(&lk->val, 0, __ATOMIC_RELEASE);

        // 开启中断，must be the last of the code
        pop_off();
}

int is_holding(spinlock_t *lk)
{
        if (lk->holder == 0)
        {
                return 0;
        }

        struct cpu *now = get_cpu();

        if (lk->holder == now)
        {
                return 1;
        }
        return 0;
}

// push_off 和 pop_off必须成对出现
// 一次push_off 必须对应一次 pop_off
// 若每次直接开启中断，则当函数嵌套且需要释放对应锁时
// 会出现问题，即将整个cpu暴露在了中断的上下文中
void push_off(void)
{
        int old = intr_get();

        // disable interrupts to prevent an involuntary context
        // switch while using get_cpu().
        intr_off();

        if (get_cpu()->noff == 0)
                get_cpu()->intena = old;
        get_cpu()->noff += 1;
}

void pop_off(void)
{
        struct cpu *c = get_cpu();
        if (intr_get())
                panic(PANIC_ERROR, "pop_off - interruptible");
        if (c->noff < 1)
                panic(PANIC_ERROR, "pop_off");
        c->noff -= 1;
        if (c->noff == 0 && c->intena)
                intr_on();
}
