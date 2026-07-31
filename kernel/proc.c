#include "proc.h"
#include "riscv.h"
#include "../include/stdint.h"
#include "printk.h"

struct cpu cpus[NCPUS];

struct cpu *get_cpu()
{
        return &cpus[r_tp()];
}

uint64_t get_cpu_id()
{
        return r_tp();
}

void init_cpu()
{
        struct cpu *now = get_cpu();
        now->ctx = NULL;
        now->tf = NULL;
        now->hart_id = r_tp();
        now->intena = 0;
        now->noff = 0;
}