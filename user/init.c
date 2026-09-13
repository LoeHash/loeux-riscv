// init.c
#include <stdio.h>
#include <ustring.h>
#include <ulib.h>
#include <ufile.h>

// 构造 argv 并 exec 指定的程序
// path: 要执行的程序路径
// 返回: -1 表示失败（exec 失败），成功则不返回
static int run_program(const char *path)
{
        // argv[0] 通常是程序名，argv 必须以 NULL 结尾
        char *argv[2];
        argv[0] = (char *)path;
        argv[1] = NULL;

        return exec(path, argv);
}

// 尝试执行 lsh（在几个可能的路径里找）
// 返回: -1 表示全部失败
static int try_exec_lsh(void)
{
        // 显式地尝试几个常见路径。
        const char *paths[] = {
            "/bin/lsh",
            "/usr/bin/lsh",
            "/lsh",
            "lsh",
            NULL};

        for (int i = 0; paths[i] != NULL; i++)
        {
                if (run_program(paths[i]) == 0)
                {
                        // 按约定 exec 成功不应返回；
                        // 但为了保险，若返回 0 也认为要退出重试。
                        return 0;
                }
        }
        return -1;
}

int main(int argc, char *argv[])
{
        printf("[init] init started, pid=%d, ppid=%d\n",
               get_pid(), get_ppid());

        // PID 1 的 init 必须持续运行，不能退出。
        // 用 fork + exec 来跑 lsh，父进程负责 wait 和重启。
        for (;;)
        {
                int pid = fork();

                if (pid < 0)
                {
                        printf("[init] fork failed\n");
                        // 简单退避，避免疯狂刷屏
                        for (volatile int i = 0; i < 10000000; i++)
                                ;
                        continue;
                }

                if (pid == 0)
                {
                        // 子进程：尝试 exec lsh
                        printf("[init] child pid=%d trying to exec lsh...\n", get_pid());

                        if (try_exec_lsh() < 0)
                        {
                                printf("[init] exec lsh failed, child exiting\n");
                                exit(EXIT_FAILURE);
                        }

                        // exec 成功不会到达这里
                        exit(EXIT_SUCCESS);
                }

                // 父进程：等待子进程退出
                int status = 0;
                int w = waitpid(pid, &status);
                if (w < 0)
                {
                        printf("[init] waitpid failed\n");
                }
                else
                {
                        printf("[init] lsh (pid=%d) exited, status=%d\n", pid, status);
                }

                // 稍作延时再重启，避免 lsh 立刻退出导致死循环刷屏
                for (volatile int i = 0; i < 50000000; i++)
                        ;
                printf("[init] restarting lsh...\n");
        }

        return 0;
}