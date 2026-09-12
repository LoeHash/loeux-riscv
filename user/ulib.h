#ifndef _INC_USER_ULIB
#define _INC_USER_ULIB

#include <stdarg.h>
#include <utype.h>
#include <umath.h>
#include <ufile.h>
#include <ustring.h>

///////////////////////////////SYSCALLS///////////////////////////////
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
#define SYSCALL_FSTAT 14
#define SYSCALL_GETDENTS 15
#define SYSCALL_PWD 16
///////////////////////////////SYSCALLS END///////////////////////////////

int pwd(char *buf, int max);
int write(int fd, void *buf, uint64_t count);
int fork();
int exec(const char *path, char **argv);
int get_pid();
int get_ppid();
int wait(int *status);
int waitpid(int pid, int *status);
int exit(int exit_code);
int read(int fd, void *buf, uint64_t count);
int chdir(const char *path);
int mkdir(const char *path);
int open(const char *path, int flags);
int close(int fd);
int fstat(int fd, struct stat *buf);
int getdents(int fd, struct dirent *buf, uint32_t count);
#endif