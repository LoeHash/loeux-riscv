#include <ulib.h>
static inline uint64_t r_tp();

int main(int argc, char **argv)
{
        uint64_t counter = 0;
        int pid = fork();
        if (pid == 0)
        {
                while (1)
                {

                        printf("hello! this is child process pid: %d, ppid: %d\n", get_pid(), get_ppid());
                        while (counter < 10000000)
                        {
                                counter++;
                        }
                        counter = 0;
                }
        }
        else
        {
                // write(1, "hello! this is parent process\n", 32);
                while (1)
                {

                        printf("hello! this is parent process pid: %d, ppid: %d\n", get_pid(), get_ppid());
                        while (counter < 10000000)
                        {
                                counter++;
                        }
                        counter = 0;
                }
        }

        return 889;
}