#include <ulib.h>

int main(int argc, char **argv)
{
        uint64_t counter = 0;

        while (1)
        {
                write(1, "hello\n", 6);
                // while (counter < 100000000)
                // {
                //         counter++;
                // }
                counter = 0;
        }

        return 889;
}