#ifndef _INC_RISCV_
#define _INC_RISCV_
#include "../include/stdint.h"
#define SATP_SV39 (8L << 60) // mode 8
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64_t)pagetable) >> 12))

static inline void sfence_vma(void)
{
        asm volatile("sfence.vma zero, zero");
}

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