#include <ulib.h>

int main(int argc, char **argv)
{
        int counter = 0;
        while(1){
                printf("init2 running, pid=%d, ppid=%d\n", get_pid(), get_ppid());
                printf("init2 argv=%s\n", argv[0]);
                while (counter < 100000000)
                {
                        counter++;
                }
                counter = 0;
        }
        return 0;
}
