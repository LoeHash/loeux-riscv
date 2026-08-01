#ifndef _INC_RISCV_
#define _INC_RISCV_
#include "../include/stdint.h"
#define SATP_SV39 (8L << 60) // mode 8
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64_t)pagetable) >> 12))

static inline uint64_t
r_sstatus()
{
        uint64_t x;
        asm volatile("csrr %0, sstatus" : "=r"(x));
        return x;
}

static inline void
w_sstatus(uint64_t x)
{
        asm volatile("csrw sstatus, %0" : : "r"(x));
}

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

#define SSTATUS_SPP (1L << 8)  // Previous mode, 1=Supervisor, 0=User
#define SSTATUS_SPIE (1L << 5) // Supervisor Previous Interrupt Enable
#define SSTATUS_UPIE (1L << 4) // User Previous Interrupt Enable
#define SSTATUS_SIE (1L << 1)  // Supervisor Interrupt Enable
#define SSTATUS_UIE (1L << 0)  // User Interrupt Enable

// Supervisor Interrupt Pending
static inline uint64_t
r_sip()
{
        uint64_t x;
        asm volatile("csrr %0, sip" : "=r"(x));
        return x;
}

static inline void
w_sip(uint64_t x)
{
        asm volatile("csrw sip, %0" : : "r"(x));
}

// Supervisor Interrupt Enable
#define SIE_SEIE (1L << 9) // external
#define SIE_STIE (1L << 5) // timer
static inline uint64_t
r_sie()
{
        uint64_t x;
        asm volatile("csrr %0, sie" : "=r"(x));
        return x;
}

static inline void
w_sie(uint64_t x)
{
        asm volatile("csrw sie, %0" : : "r"(x));
}

// we use the tp reg to save the cpu hart id
static inline uint64_t
r_tp()
{
        uint64_t x;
        asm volatile("mv %0, tp" : "=r"(x));
        return x;
}

static inline void
w_tp(uint64_t x)
{
        asm volatile("mv tp, %0" : : "r"(x));
}

// 设备是否开启了中断
static inline int intr_get()
{
        uint64_t x = r_sstatus();
        return (x & SSTATUS_SIE) != 0;
}

// enable device interrupts
static inline void
intr_on()
{
        w_sstatus(r_sstatus() | SSTATUS_SIE);
}

// disable device interrupts
static inline void
intr_off()
{
        w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

static inline void w_sp(uint64_t x)
{
        asm volatile("mv sp, %0" : : "r"(x));
}

static inline uint64_t r_sp()
{
        uint64_t x;
        asm volatile("mv %0, sp" : "=r"(x));
        return x;
}

static inline uint64_t
r_stvec()
{
        uint64_t x;
        asm volatile("csrr %0, stvec" : "=r"(x));
        return x;
}

static inline void
w_stvec(uint64_t x)
{
        asm volatile("csrw stvec, %0" : : "r"(x));
}
#endif