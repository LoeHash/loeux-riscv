#ifndef _INC_SPINLOCK_
#define _INC_SPINLOCK_
#include "../include/stdint.h"
#include "proc.h"
struct spinlock
{
        uint64_t val;
        struct cpu *holder;
};

typedef struct spinlock spinlock_t;
int is_holding(spinlock_t *lk);
void push_off(void);
void pop_off(void);
void init_spinlock(spinlock_t *lk);
void acquire(spinlock_t *lk);
void release(spinlock_t *lk);

#endif