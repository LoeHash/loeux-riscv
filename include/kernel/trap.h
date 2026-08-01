#ifndef _INC_TRAP
#define _INC_TRAP
#include <stdint.h>

// 异常处理需要使用到CSR寄存器
//      CSR	        作用
//      stvec   	陷阱向量基址，指向异常处理入口函数
//      sscratch        临时保存寄存器用，通常存内核栈指针
//      sepc    	保存异常发生时的 PC
//      scause  	异常/中断的原因码
//      stval   	异常的附加信息（如缺页地址）
//      sstatus 	S-mode 状态（含中断使能位 SIE）
//      sie     	中断使能寄存器
//      sip     	中断挂起寄存器

void init_kernel_trap_vec();
void kernel_trap_hanlder(uint64_t scause, uint64_t sepc, uint64_t stval);
void user_trap_hanlder(uint64_t scause, uint64_t sepc, uint64_t stval);

#endif