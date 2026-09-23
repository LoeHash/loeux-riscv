#include <ulib.h>
#include <stdio.h>

int main(int argc, char* argv[])
{
	printf("colortest ready, press keys:\n");
	printf("\033[31mred text\033[0m\n");
	printf("\033[32;40mgreen on black\033[0m\n");
	printf("%c[33myellow%c[0m\n", 27, 27);
	return 0;
}
