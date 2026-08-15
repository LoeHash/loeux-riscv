#include <syscall.h>
#include <type.h>
#include <vm.h>
#include <spinlock.h>
#include <proc.h>

static syscall_func_t syscalls[] = {
    [0] 0 // syscall id = 0
          // []
};

void syscall()
{
        while (1)
                ;
}