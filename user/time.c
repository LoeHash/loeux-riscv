#include <stdio.h>
#include <ulib.h>

int main(int argc, char const *argv[])
{
        uint64_t tick = get_tick();
        printf("tick: %llu\n", tick);
        return 0;
}
