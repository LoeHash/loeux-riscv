#ifndef _INC_SYSCALL_
#define _INC_SYSCALL_
#include <type.h>
#define ARR_LEN(arr) (sizeof(arr) / sizeof(arr[0]))

#define BUFSZ 4096
#define MAX_PATH_LEN 128
#define MAX_ARGS 32

#define SYSCALL_EXIT 1
#define SYSCALL_WRITE 2
#define SYSCALL_FORK 3
#define SYSCALL_EXEC 4
#define SYSCALL_GETPID 5
#define SYSCALL_GETPPID 6
#define SYSCALL_WAIT 7
#define SYSCALL_WAITPID 8
#define SYSCALL_READ 9
#define SYSCALL_CHDIR 10
#define SYSCALL_MKDIR 11
#define SYSCALL_OPEN 12
#define SYSCALL_CLOSE 13

typedef uint64_t (*syscall_func_t)(void);

int copy_data_addr(uint64_t addr, uint64_t *ip);
int copy_data_str_out(uint64_t addr, char *buf, int max);
int copy_data_str(uint64_t addr, char *buf, int max);
void get_arg_addr(int n, uint64_t *buf);
void get_arg_int(int n, int *buf);
void syscall();
void do_build_user_path(char *des_path, char *u_path, char *cwd);
#endif
