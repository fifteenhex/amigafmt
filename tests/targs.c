/* ReadArgs: run with a template and arguments, print what it returns */
#include "t.h"

int main(u32 arglen, char *args)
{
	static const char tmpl[] = "FROM/M,TO/A,QUIET/S,NUM/K/N,TEXT/K,TOG/T";
	s32 a[6] = { 0, 0, 0, 0, 0, -1 };
	void *rd;
	int i;

	printf("arglen %ld args [", arglen);
	call(DOSBase, LVOWrite, "d1d2d3", call(DOSBase, LVOOutput, ""), args,
	     arglen - 1);
	printf("]\n");
	rd = (void *)call(DOSBase, LVOReadArgs, "d1d2d3", tmpl, a, 0);
	if (!rd) {
		char buf[100];
		call(DOSBase, LVOFault, "d1d2d3d4",
		     call(DOSBase, LVOIoErr, ""), "ReadArgs", buf, 100);
		printf("%s\n", buf);
		return 10;
	}
	if (a[0])
		for (i = 0; ((char **)a[0])[i]; i++)
			printf("FROM[%ld] = \"%s\"\n", i, ((char **)a[0])[i]);
	printf("TO = \"%s\"\n", a[1]);
	printf("QUIET = %ld\n", a[2]);
	if (a[3])
		printf("NUM = %ld\n", *(s32 *)a[3]);
	if (a[4])
		printf("TEXT = \"%s\"\n", a[4]);
	printf("TOG = %ld\n", a[5]);
	call(DOSBase, LVOFreeArgs, "d1", rd);
	return 0;
}
