#include <type.h>
#include <trap.h>
#include <stdint.h>
#include <printk.h>
#include <proc.h>
#include <panic.h>
#include <riscv.h>
#include <sbi.h>
#include <timer.h>
#include <syscall.h>

extern uint64_t main_core;
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

        // 解决页表缓存问题
        // sfence_vma();

        uint64_t sstatus = r_sstatus();
        struct task_struct *ts = get_task();
        if (r_sstatus_spp() != 1)
        {
                printk("KERNEL TRAP RAW: tp=%lx hart=%d scause=%lx sepc=%lx satp=%lx stvec=%lx sstatus=%lx\n",
                       r_tp(),
                       get_cpu_id(),
                       scause,
                       sepc,
                       r_satp(),
                       r_stvec(),
                       r_sstatus());
                printk("the main cpu: %0#lx\n", main_core);
                printk("wrong with the cpu id: %d\n", get_cpu_id());
                panic(PANIC_ERROR, "kernel_trap_hanlder: NOT FROM THE KERNEL!\n");
        }

        if (scause == CLINT_INTERRUPT_SCAUSE)
        {
                // 时钟中断
                // 在内核态里的时钟中断
                do_timer_tick();
                sbi_set_timer(rdtime() + (BASE_FREQUENCY / TASK_CPU_SLIP_FACTOR));
                if (ts == 0)
                {
                        w_sepc(sepc);
                        w_sstatus(sstatus);
                        return;
                }

                ts->stvec = (uint64_t)kernel_trap_vec; // 当前 task 处于 kernel trap

                ts->trap_sepc = sepc;       // 保存这次 kernel trap 的返回 PC
                ts->trap_sstatus = sstatus; // 保存这次 kernel trap 的 sstatus
                ts->trap_ctx_valid = 1;     // 标记 task 当前存在 kernel trap continuation

                w_stvec(ts->stvec); // 当前 hart 使用 kernel trap vector

                yield(); // task 可能迁移到其他 hart

                task_restore_stvec(ts); // yield 返回后恢复 task 自己的 stvec

                if (ts->trap_ctx_valid)
                {
                        w_sepc(ts->trap_sepc);       // 恢复 task 原来的 kernel trap PC
                        w_sstatus(ts->trap_sstatus); // 恢复 task 原来的 kernel trap sstatus
                }

                ts->trap_ctx_valid = 0; // kernel trap continuation 已经完成

                return;
        }
        printk("the kernel_pt %0#lx\n", kernel_pt);

        printk(
            "KERNEL TRAP ENTRY: "
            "hart=%d "
            "scause=%lx "
            "sepc=%lx "
            "stval=%lx "
            "sstatus=%lx "
            "SPP=%d "
            "SIE=%d "
            "satp=%lx "
            "stvec=%lx\n",
            get_cpu_id(),
            scause,
            sepc,
            stval,
            sstatus,
            (int)((sstatus & SSTATUS_SPP) != 0),
            (int)((sstatus & SSTATUS_SIE) != 0),
            r_satp(),
            r_stvec());

        printk("Wrong with the cpu id: %d\n", get_cpu_id());
        printk("   scause 保存异常发生时的 PC: %0#lx\n", scause);
        printk("   sepc   保存异常发生时的 PC: %0#lx\n", sepc);
        printk("   stval  异常的附加信息:%0#lx\n", stval);

        while (1)
        {
                /* code */
        }
        // intr_on();
}

/// @brief
/// @param scause 保存异常发生时的 PC
/// @param sepc   保存异常发生时的 PC
/// @param stval  异常的附加信息（如缺页地址）
uint64_t user_trap_hanlder(uint64_t scause, uint64_t sepc, uint64_t stval)
{
        intr_off();

        /* Now get the task the regular way (this may deref cpus[r_tp()].ts) */
        struct task_struct *ts = get_task();

        if (ts == 0)
        {
                panic(PANIC_ERROR, "usertrap: error! ts == NULL\n");
        }
        struct trapframe *utf = ts->utf;

        // 切换当前trap
        task_set_stvec(ts, (uint64_t)kernel_trap_vec);
        ts->utf->sepc = sepc;

        // 状态判断
        if (r_sstatus_spp() == 1)
        {
                panic(PANIC_ERROR, "usertrap: from s-mode!\n");
        }

        if (scause == 8) // syscall
        {
                // 取出a7
                if (ts->dead)
                {
                        // we need the exit
                        while (1)
                                ;
                }

                ts->utf->sepc += 4; // ecall 指令为 4字节
                ts->in_syscall = true;
                intr_on();
                syscall();
                intr_off();
                ts->in_syscall = false;
        }
        else if (scause == CLINT_INTERRUPT_SCAUSE)
        {
                do_timer_tick();
                /*
                 * !!! DO NOT REMOVE !!!
                 *
                 * Timer is one-shot. Every timer interrupt MUST re-arm the
                 * next timer before returning from the trap.
                 *
                 * Without this call:
                 *
                 * timer -> trap -> return -> timer -> trap -> ...
                 *
                 *
                 * The CPU may be trapped in a continuous timer-interrupt loop. */
                sbi_set_timer(rdtime() + (BASE_FREQUENCY / TASK_CPU_SLIP_FACTOR));
                yield();
        }
        else // else.
        {
                // wrong
                printk("the ssp %0#lx\n", r_sstatus_spp());
                printk("Wrong with the cpu id: %d\n", get_cpu_id());
                printk("   scause 保存异常发生时的 PC: %0#lx\n", scause);
                printk("   sepc   保存异常发生时的 PC: %0#lx\n", sepc);
                printk("   stval  异常的附加信息:%0#lx\n", stval);

                ts->dead = 1;

                // 暂时直接停住，方便调试
                while (1)
                        ;
        }

        setup_return_trapframe(ts);
        // intr_on(); 不要开启，否则会创造出一个窗口期
        return (uint64_t)(MAKE_SATP(ts->pg));
}

void setup_return_trapframe(struct task_struct *ts)
{
        intr_off();
        // 准备返回
        // 1. 提前设置栈指针
        ts->utf->kernel_sp = (uint64_t)TASK_KERNEL_STACK(ts - tasks);
        // 2. 准备内核页表
        ts->utf->kernel_satp = MAKE_SATP(kernel_pt);
        // 3. 设置下次进入的函数
        ts->utf->kernel_trap = (uint64_t)user_trap_hanlder;
        // 4. 设置cpu id
        ts->utf->kernel_hartid = r_tp();
        // 5. 设置蹦床, 为下次进入做准备
        task_set_stvec(ts, (uint64_t)TRAMPOLINE);
        // 6. 清空状态
        w_sstatus((r_sstatus() & ~SSTATUS_SPP) | SSTATUS_SPIE);
        // 7. 设置返回pc
        w_sepc(ts->utf->sepc);
}

void init_kernel_trap_vec()
{
        w_stvec((uint64_t)kernel_trap_vec);
}