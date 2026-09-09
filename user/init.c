#include <ulib.h>

int main(int argc, char **argv)
{
        while (1)
        {
                int pid = fork();
                if (pid == 0)
                {
                        printf("child %d (ppid %d) running, will exit\n", get_pid(), get_ppid());
                        exit(42);
                }
                else
                {
                        int status = 0;
                        int wpid = wait(&status);
                        printf("parent %d reaped child %d, exit status=%d\n", get_pid(), wpid, status);
                }
        }

        return 0;
}
