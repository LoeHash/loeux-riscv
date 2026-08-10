#ifndef _INC_TYPE
#define _INC_TYPE
#include <stdint.h>
#define NULL ((void *)0)

struct spinlock
{
        uint64_t val;
        struct cpu *holder;
};

typedef unsigned long phys_addr_t;
typedef unsigned long vir_addr_t;
typedef struct spinlock spinlock_t;

/// @brief 表示一页的管理信息
struct page
{
        phys_addr_t paddr;
        int32_t refcount;
        uint64_t flags;
        struct page *prev;
        struct page *next;
};

struct gloal_memory_descriptor
{
        struct page *pg;
        struct page *kernel_head;  // 保留节点的头
        struct page *kernel_tail;  // 保留节点的尾
        struct page *fdt_head;     // fdt设备树保留节点的头
        struct page *fdt_tail;     // fdt设备树保留节点的尾
        struct page *free_head;    // 空闲节点的头
        struct page *free_tail;    // 空闲节点的尾
        phys_addr_t free_start_at; // 绝对，空闲起始
        phys_addr_t free_end_at;   // 绝对，空闲结束
        uint64_t page_length;      // 页表长度
};

struct bank
{
        phys_addr_t base;
        phys_addr_t size;
};

// 内核现场
// 不管是什么原因
// 进程切换时必然发生在内核态中
// switch函数将实现：保存当前的context要求的寄存器到对应的位置
//                 同时加载新的寄存器到当前寄存器，同时保存到cpu结构体的context
struct context
{
        /*  0 */ uint64_t ra;
        /*  8 */ uint64_t sp;
        // callee-saved
        /* 16 */ uint64_t s0;
        /* 24 */ uint64_t s1;
        /* 32 */ uint64_t s2;
        /* 40 */ uint64_t s3;
        /* 48 */ uint64_t s4;
        /* 56 */ uint64_t s5;
        /* 64 */ uint64_t s6;
        /* 72 */ uint64_t s7;
        /* 80 */ uint64_t s8;
        /* 88 */ uint64_t s9;
        /* 96 */ uint64_t s10;
        /* 104 */ uint64_t s11;
};

// 用户进程现场
// 不同于kenerl!
struct trapframe
{
        /*   0 */ uint64_t kernel_satp;   // kernel page table
        /*   8 */ uint64_t kernel_sp;     // 每一个进程对应的唯一的内核栈
        /*  16 */ uint64_t kernel_trap;   // usertrap()
        /*  24 */ uint64_t kernel_hartid; // saved kernel tp
        /*  32 */ uint64_t sepc;          // 不要在刚枪的时候右键。。。
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

enum TASK_STATE
{
        UNINITLIZED,
        INITLIZED,
        RUNNABLE,
        RUNNING,
        BLOCKED,
        SLEEP,
        DEAD
};
typedef uint64_t pte;
typedef uint64_t *page_table;

struct task_struct
{
        struct trapframe *utf; // 用户现场
        enum TASK_STATE state; // 状态
        spinlock_t lk;         // 进程锁

        page_table pg; // 进程页表

        struct context ctx;         // 各个进程的内核态现场
        int pid;                    // 进程id
        struct task_struct *parent; // 父亲进程
        char name[32];              // 进程name
        int return_val;             // 进程运行完毕后的返回值
};

struct cpu
{
        struct task_struct *ts; // 当前cpu运行的任务
        struct context ctx;     // 内核态的现场
        uint64_t hart_id;

        int noff;   // 调用pushoff的深度
        int intena; // 第一次push_off 的中断状态
};

#endif