#include <ulib.h>

int main(int argc, char **argv)
{
        int counter = 0;
        while (1)
        {
                int pid = fork();

                if (pid == 0)
                {

                        // int ret = exec("/_init2", (char *[]){"/weww", NULL});
                        // printf("%d\n", ret);
                        printf("child %d (ppid %d) will go to read\n", get_pid(), get_ppid());
                        char buf[32];
                        int ret = read(0, buf, sizeof(buf));
                        printf("child %d (ppid %d) read %d bytes, buf=%s\n", get_pid(), get_ppid(), ret, buf);
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
