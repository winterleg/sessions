#include "stdio.h"

const char* paths[] = {
	"/home/hiver/Desktop/",
	"/home/hiver/.config/",
	"/home/hiver/Downloads/"
};

int
main()
{
	for (int i = 0; i < 3; i++)
	{
		printf("Value: %s\n", paths[i]);
	}
	return 0;
}
