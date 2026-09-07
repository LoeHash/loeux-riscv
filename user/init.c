#include <ulib.h>
static inline uint64_t r_tp();

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
static inline uint64_t r_tp()
{
        uint64_t x;
        asm volatile("mv %0, tp" : "=r"(x));
        return x;
}