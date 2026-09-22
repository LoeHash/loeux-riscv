// init.c
#include <stdio.h>
#include <ustring.h>
#include <ulib.h>
#include <ufile.h>

// 尝试执行 lsh：交给 ulib 的 execvp 在 PATH 里搜索。
// 返回: -1 表示所有候选目录都没找到（exec 成功不返回）
static int try_exec_lsh(void)
{
	// argv[0] 是程序名，argv 必须以 NULL 结尾
	char* argv[2];
	argv[0] = "lsh";
	argv[1] = NULL;

	return execvp("lsh", argv);
}

int main(int argc, char* argv[])
{
	printf("[init] init started, pid=%d, ppid=%d\n", get_pid(), get_ppid());

	// PID 1 的 init 必须持续运行，不能退出。
	// 用 fork + exec 来跑 lsh，父进程负责 wait 和重启。
	for (;;) {
		int pid = fork();

		if (pid < 0) {
			printf("[init] fork failed\n");
			// 简单退避，避免疯狂刷屏
			for (volatile int i = 0; i < 10000000; i++)
				;
			continue;
		}

		if (pid == 0) {
			// 子进程：尝试 exec lsh
			printf("[init] child pid=%d trying to exec lsh...\n",
			       get_pid());

			if (try_exec_lsh() < 0) {
				printf(
				    "[init] exec lsh failed, child exiting\n");
				exit(EXIT_FAILURE);
			}

			// exec 成功不会到达这里
			exit(EXIT_SUCCESS);
		}

		// 父进程：等待子进程退出
		int status = 0;
		int w = waitpid(pid, &status);
		if (w < 0) {
			printf("[init] waitpid failed\n");
		} else {
			printf("[init] lsh (pid=%d) exited, status=%d\n",
			       pid,
			       status);
		}

		// 稍作延时再重启，避免 lsh 立刻退出导致死循环刷屏
		for (volatile int i = 0; i < 50000000; i++)
			;
		printf("[init] restarting lsh...\n");
	}

	return 0;
}