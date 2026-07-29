#ifndef _INC_RISCV_
#define _INC_RISCV_
#include "../include/stdint.h"

static inline void w_satp(uint64_t x)
{
        asm volatile("csrw satp, %0" : : "r"(x));
}

static inline uint64_t r_satp()
{
        uint64_t x;
        asm volatile("csrr %0, satp" : "=r"(x));
        return x;
}

#endif