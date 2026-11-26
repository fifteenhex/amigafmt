/* calling a vector amigafmt does not implement must stop the program */
#include "t.h"

int main(u32 arglen, char *args)
{
	printf("before\n");
	call(DOSBase, -930, "");        /* dos.library CliInitNewcli */
	printf("after (should not appear)\n");
	return 0;
}
