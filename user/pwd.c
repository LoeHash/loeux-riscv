#include <ulib.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
        char buf[512];
        int ret;

        ret = pwd(buf, sizeof(buf));
        if (ret < 0)
        {
                printf("pwd: error\n");
                exit(1);
        }
        printf("%s\n", buf);
        exit(0);
}