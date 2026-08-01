#include "trap.h"
#include "../include/stdint.h"
#include "printk.h"
#include "proc.h"
#include "riscv.h"

extern char kernel_trap_vec[];

/// @brief
/// @param scause 保存异常发生时的 PC
/// @param sepc   保存异常发生时的 PC
/// @param stval  异常的附加信息（如缺页地址）
void kernel_trap_hanlder(uint64_t scause, uint64_t sepc, uint64_t stval)
{
        intr_off();

        printk("Wrong with the cpu id: %d\n", get_cpu_id());
        printk("   scause 保存异常发生时的 PC: %0#lx\n", scause);
        printk("   sepc   保存异常发生时的 PC: %0#lx\n", sepc);
        printk("   stval  异常的附加信息:%0#lx\n", stval);

        while (1)
        {
        }

        intr_on();
}

void init_kernel_trap_vec()
{
        printk("fuck, %0#lx\n", (uint64_t)kernel_trap_vec);
        w_stvec((uint64_t)kernel_trap_vec);
}