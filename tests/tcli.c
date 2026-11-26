/* dos.library tests:
     tcli vars           GetVar/SetVar, local and global */
#include "t.h"

static int word(const char **p, char *w, int max)
{
	int n = 0;

	while (**p == ' ')
		(*p)++;
	while (**p && **p != ' ' && **p != '\n' && n < max - 1)
		w[n++] = *(*p)++;
	w[n] = 0;
	return n;
}

int main(u32 arglen, char *args)
{
	const char *p = args;
	char cmd[32], arg[128];

	word(&p, cmd, sizeof(cmd));
	word(&p, arg, sizeof(arg));
	if (!strcmp(cmd, "vars")) {
		char buf[64];
		s32 n;
		call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", "local", -1, 0);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0);
		printf("local %ld %s\n", n, n >= 0 ? buf : "-");
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0x100);
		printf("global only %ld\n", n);
		call(DOSBase, LVOSetIoErr, "d1", 0);
		n = call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", "x", 0x7fffffff, 0);
		printf("no memory %ld, IoErr %ld\n", n,
		       (s32)call(DOSBase, LVOIoErr, ""));
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0);
		printf("still %ld %s\n", n, n >= 0 ? buf : "-");
		call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", "global\nsecond line", -1, 0x100);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0x100);
		printf("global %ld %s\n", n, buf);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 4, 0x100);
		printf("truncated %ld %s\n", n, buf);
		call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", 0, 0, 0x100);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0x100);
		printf("deleted %ld\n", n);
		return 0;
	}
	return 10;
}
