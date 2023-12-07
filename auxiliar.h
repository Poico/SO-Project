#include <string.h>

char *pathCombine(char *dest, char *src)
{
	char *destEnd = &dest[strlen(dest) - 1];

	if (*destEnd != '/' && *src != '/') //No separator
	{
		*(destEnd+1) = '/';
		*(destEnd+2) = '\0';
	}
	else if (*destEnd == '/' && *src == '/') //Repeated separator
	{
		*destEnd = '\0';
	}

	strcat(dest, src);
	return dest;
}
