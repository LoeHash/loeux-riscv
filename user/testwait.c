#include <stdio.h>
#include <ulib.h>
#include <ustring.h>

int main(int argc, char const *argv[])
{
        int pid = get_pid();

        printf("this is my pid: %d\n", pid);
        printf("[%d] going to run.....\n", pid);
        while (1)
        {
        }
        exit(0);

        return 0;
}
