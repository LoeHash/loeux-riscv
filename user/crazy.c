#include <ulib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
        int counter = 10000;
        while (counter > 0)
        {
                int a = fork();
                if (a == 0)
                {
                        execvp("/slabtest", NULL);
                }
                counter--;
        }
}