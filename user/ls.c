#include <ulib.h>

int main(int argc, char **argv)
{
        printf("ls: list files\n");
        *(int *)(0x8f8f8f8) = 48;
        return 0;
}