#ifndef __INC_PROC
#define __INC_PROC
#include <memlayout.h>
#include <vm.h>
#include <stdint.h>
#include <type.h>

#define NTASKS 128
#define NCPUS 4
#define USER_PROG_LOAD_AT 0x1000 // 用户程序加载的位置
#define MAX_ARG_NUM 64
extern struct task_struct tasks[NTASKS];
extern struct cpu cpus[NCPUS];

struct cpu *get_cpu();
uint64_t get_cpu_id();
void init_cpu();
void init_tasks();
void scheduler() __attribute__((noreturn));
struct task_struct *get_task();
void yield();
void sched();
void swtch();
void free_task(struct task_struct *);
struct task_struct *alloc_task();
void free_task_pgtable(page_table pagetable, uint64_t sz);
page_table create_task_pgtable(struct task_struct *ts);
int kexec(char *path, char **argv);
void init_user();
void set_cwd(struct task_struct *ts, char *path);
#endif