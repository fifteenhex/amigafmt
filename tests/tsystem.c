/* System()/Execute(): AmigaOS programs in-process, others via /bin/sh */
#include "t.h"

int main(u32 arglen, char *args)
{
	char buf[64];
	u32 fh;
	s32 n;

	printf("System(hello >T:af-sys.out) = %ld\n",
	       call(DOSBase, LVOSystemTagList, "d1d2", "hello >T:af-sys.out", 0));
	fh = call(DOSBase, LVOOpen, "d1d2", "T:af-sys.out", 1005);
	n = fh ? call(DOSBase, LVORead, "d1d2d3", fh, buf, 63) : -1;
	if (fh)
		call(DOSBase, LVOClose, "d1", fh);
	call(DOSBase, LVODeleteFile, "d1", "T:af-sys.out");
	buf[n > 0 ? n : 0] = 0;
	printf("it wrote: %s", buf);
	printf("System(targs a b TO c) = %ld\n",
	       call(DOSBase, LVOSystemTagList, "d1d2", "targs a b TO c", 0));
	printf("System(echo from-linux) = %ld\n",
	       call(DOSBase, LVOSystemTagList, "d1d2", "echo from-linux", 0));
	printf("System(exit 7) = %ld\n",
	       call(DOSBase, LVOSystemTagList, "d1d2", "exit 7", 0));
	printf("Execute(hello) = %ld\n",
	       call(DOSBase, LVOExecute, "d1d2d3", "hello", 0, 0));
	return 0;
}
