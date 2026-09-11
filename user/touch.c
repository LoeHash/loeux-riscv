#include <ulib.h>

int main(int argc, char **argv)
{
        int fd;

        if (argc != 2)
        {
                printf("usage: touch <path>\n");
                return -1;
        }

        fd = open(argv[1], O_CREAT);

        if (fd < 0)
        {
                printf("touch: failed to create '%s'\n", argv[1]);
                return -1;
        }

        close(fd);

        return 0;
}