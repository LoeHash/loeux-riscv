#ifndef __INC_PROC
#define __INC_PROC
#include "../include/stdint.h"
#include "../mm/memlayout.h"

// 内核现场
struct context
{
        uint64_t ra;
        uint64_t sp;

        // callee-saved
        uint64_t s0;
        uint64_t s1;
        uint64_t s2;
        uint64_t s3;
        uint64_t s4;
        uint64_t s5;
        uint64_t s6;
        uint64_t s7;
        uint64_t s8;
        uint64_t s9;
        uint64_t s10;
        uint64_t s11;
};

// 用户进程现场
// 不同于kenerl!
struct trapframe
{
        /*   0 */ uint64_t kernel_satp;   // kernel page table
        /*   8 */ uint64_t kernel_sp;     // top of process's kernel stack
        /*  16 */ uint64_t kernel_trap;   // usertrap()
        /*  24 */ uint64_t epc;           // saved user program counter
        /*  32 */ uint64_t kernel_hartid; // saved kernel tp
        /*  40 */ uint64_t ra;
        /*  48 */ uint64_t sp;
        /*  56 */ uint64_t gp;
        /*  64 */ uint64_t tp;
        /*  72 */ uint64_t t0;
        /*  80 */ uint64_t t1;
        /*  88 */ uint64_t t2;
        /*  96 */ uint64_t s0;
        /* 104 */ uint64_t s1;
        /* 112 */ uint64_t a0;
        /* 120 */ uint64_t a1;
        /* 128 */ uint64_t a2;
        /* 136 */ uint64_t a3;
        /* 144 */ uint64_t a4;
        /* 152 */ uint64_t a5;
        /* 160 */ uint64_t a6;
        /* 168 */ uint64_t a7;
        /* 176 */ uint64_t s2;
        /* 184 */ uint64_t s3;
        /* 192 */ uint64_t s4;
        /* 200 */ uint64_t s5;
        /* 208 */ uint64_t s6;
        /* 216 */ uint64_t s7;
        /* 224 */ uint64_t s8;
        /* 232 */ uint64_t s9;
        /* 240 */ uint64_t s10;
        /* 248 */ uint64_t s11;
        /* 256 */ uint64_t t3;
        /* 264 */ uint64_t t4;
        /* 272 */ uint64_t t5;
        /* 280 */ uint64_t t6;
};

#define NCPUS 4

struct cpu
{
        struct trapframe *tf; // 当前cpu的所有通用寄存器上下文
        struct context *ctx;  // 内核态的现场
        uint64_t hart_id;

        int noff;   // 调用pushoff的深度
        int intena; // 第一次push_off 的中断状态
};

extern struct cpu cpus[NCPUS];

struct cpu *get_cpu();
uint64_t get_cpu_id();
void init_cpu();
#endif