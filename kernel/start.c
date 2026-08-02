#include <sbi.h>
#include <fdt.h>
#include <printk.h>
#include <spinlock.h>
#include <riscv.h>
#include <memory.h>
#include <memlayout.h>
#include <vm.h>
#include <proc.h>
#include <trap.h>

extern char _sec_entry64[];

unsigned long ft_base_addr;
__attribute__((aligned(16))) char _stack_[4096 * 4 * NCPUS];
uint64_t main_core = 0;
uint8_t kernel_inited = 0;
void secondary_start(uint64_t hart_id, uint64_t data_addr);

void kstart(unsigned long hart_id, unsigned long ft_addr)
{
        // write tp to save ours hart id to
        // get cput struct in anywhere
        w_tp(hart_id);
        init_cpu();
        main_core = hart_id;

        // 绕过所有函数调用，直接测锁
        // fdt solve.
        init_fdt(ft_addr);
        init_memory();
        // 构建内核页表 同時啓動mmu
        init_kvmmap();
        // 开启内核中断异常处理
        init_kernel_trap_vec();
        // 初始化task
        init_tasks();

        __atomic_store_n(&kernel_inited, 1, __ATOMIC_RELEASE);

        // 唤醒多核
        for (int i = 0; i < NCPUS; i++)
        {
                if (i == main_core)
                {
                        continue;
                }
                sbi_hart_start(i, (uint64_t)_sec_entry64, (uint64_t)NULL);
        }

        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        scheduler();
}

void secondary_start(uint64_t hart_id, uint64_t data_addr)
{
        while (!__atomic_load_n(&kernel_inited, __ATOMIC_ACQUIRE))
                ;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        w_tp(hart_id);
        init_cpu();
        init_kvmhart();
        init_kernel_trap_vec();

        printk("secondary start! hart id: %d\n", hart_id);

        scheduler();
}