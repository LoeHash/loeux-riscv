#include <type.h>
#include <trap.h>
#include <stdint.h>
#include <printk.h>
#include <proc.h>
#include <panic.h>
#include <riscv.h>
#include <sbi.h>
#include <timer.h>

extern char kernel_trap_vec[];
extern char _trampoline_jump[];
extern char _trampoline_ret[];

/// @brief
/// @param scause 保存异常发生时的 PC
/// @param sepc   保存异常发生时的 PC
/// @param stval  异常的附加信息（如缺页地址）
void kernel_trap_hanlder(uint64_t scause, uint64_t sepc, uint64_t stval)
{
        intr_off();

        if (r_sstatus_spp() != 1)
        {
                panic(PANIC_ERROR, "kernel_trap_hanlder: NOT FOMR KERNEL!\n");
        }

        if (scause == CLINT_INTERRUPT_SCAUSE)
        {
                // 时钟中断
                // 在内核态里的时钟中断
                printk("发生时钟中断! hart id: %d\n", get_cpu_id());
                do_timer_tick();

                sbi_set_timer(rdtime() + (BASE_FREQUENCY / TASK_CPU_SLIP_FACTOR));
        }
        else
        {
                printk("Wrong with the cpu id: %d\n", get_cpu_id());
                printk("   scause 保存异常发生时的 PC: %0#lx\n", scause);
                printk("   sepc   保存异常发生时的 PC: %0#lx\n", sepc);
                printk("   stval  异常的附加信息:%0#lx\n", stval);
                while (1)
                {
                        /* code */
                }
        }

        intr_on();
}

/// @brief
/// @param scause 保存异常发生时的 PC
/// @param sepc   保存异常发生时的 PC
/// @param stval  异常的附加信息（如缺页地址）
uint64_t user_trap_hanlder(uint64_t scause, uint64_t sepc, uint64_t stval)
{
        intr_off();

        // 获取当前的TAKS
        struct task_struct *ts = get_task();

        if (ts == 0)
        {
                panic(PANIC_ERROR, "usertrap: error!\n");
        }

        struct trapframe *utf = ts->utf;

        // 切换当前trap
        w_stvec((uint64_t)kernel_trap_vec);
        ts->utf->sepc = sepc;

        // 状态判断
        if (r_sstatus_spp() == 1)
        {
                panic(PANIC_ERROR, "usertrap: from s-mode!\n");
        }

        if (scause == 8) // syscall
        {
                ts->utf->sepc += 4; // ecall 指令为 4字节
        }
        else
        {
                // wrong
                printk("Wrong with the cpu id: %d\n", get_cpu_id());
                printk("   scause 保存异常发生时的 PC: %0#lx\n", scause);
                printk("   sepc   保存异常发生时的 PC: %0#lx\n", sepc);
                printk("   stval  异常的附加信息:%0#lx\n", stval);
        }

        intr_on();
        setup_return_trapframe(ts);

        return (uint64_t)(MAKE_SATP(ts->pg));
}

void setup_return_trapframe(struct task_struct *ts)
{
        intr_off();
        // 准备返回
        // 1. 提前设置栈指针
        ts->utf->kernel_sp = (uint64_t)TASK_KERNEL_STACK(ts - tasks);
        // 2. 准备内核页表
        ts->utf->kernel_satp = (uint64_t)kernel_pt;
        // 3. 设置下次进入的函数
        ts->utf->kernel_trap = (uint64_t)user_trap_hanlder;
        // 4. 设置cpu id
        ts->utf->kernel_hartid = r_tp();
        // 5. 设置蹦床, 为下次进入做准备
        w_stvec((uint64_t)(TRAMPOLINE + (_trampoline_jump - TRAMPOLINE)));
        // 6. 清空状态
        w_sstatus((r_sstatus() & ~SSTATUS_SPP) | SSTATUS_SPIE);
        // 7. 设置返回pc
        w_sepc(ts->utf->sepc);
}

void init_kernel_trap_vec()
{
        w_stvec((uint64_t)kernel_trap_vec);
}