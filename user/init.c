#include <ulib.h>

int main(int argc, char **argv)
{
        int counter = 0;
        while (1)
        {
                int pid = fork();

                if (pid == 0)
                {

                        int ret = exec("/_init2", (char *[]){"/weww", NULL});
                        printf("%d\n", ret);
                        printf("child %d (ppid %d) running, will exit\n", get_pid(), get_ppid());
                        while (counter < 100000000)
                        {
                                counter++;
                        }
                        counter = 0;
                        exit(8848);
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
