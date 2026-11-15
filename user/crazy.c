#include <ulib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
        int counter = 3;
        while (counter > 0)
        {
                int a = fork();
                if (a == 0)
                {
                        execvp("ls", NULL);
                }
                counter--;
        }
}