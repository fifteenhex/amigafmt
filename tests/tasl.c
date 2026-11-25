/*
 * asl requesters on a custom screen: tasl file|font|mode
 * Opens a hires screen and a window on it, then the requester asked for
 * with ASLxx_Window set (so it must come up on that screen), and prints
 * what it answered.  Needs wbde, and somebody (or a script) to answer.
 */
#include "t.h"
#include "lvo_gui.h"

#define ASL_TB 0x80080000
#define SA_Dummy (0x80000000 + 32)
#define WA_Dummy (0x80000000 + 99)

static void *IntuitionBase, *AslBase;

int main(u32 arglen, char *args)
{
	u32 st[] = { SA_Dummy + 3, 640, SA_Dummy + 4, 200, SA_Dummy + 5, 3,
		     SA_Dummy + 0x12, 0x19000, SA_Dummy + 8, (u32)"tasl", 0 };
	u32 wt[] = { WA_Dummy + 13, 0, WA_Dummy + 2, 12, WA_Dummy + 3, 300, WA_Dummy + 4, 60,
		     WA_Dummy + 11, (u32)"parent", 0 };
	u32 at[] = { ASL_TB + 2, 0, ASL_TB + 9, (u32)"SYS:", ASL_TB + 60, 1, 0 };
	char kind[8] = "file";
	void *s, *w;
	u8 *r;
	u32 type = 0, ok, i, n = 0;

	for (i = 0; i < arglen && args[i] == ' '; i++)
		;
	for (; i < arglen && args[i] > ' ' && n < 7; i++)
		kind[n++] = args[i];
	if (n)
		kind[n] = 0;
	if (!strcmp(kind, "font"))
		type = 1;
	else if (!strcmp(kind, "mode"))
		type = 2;
	IntuitionBase = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "intuition.library", 39);
	AslBase = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "asl.library", 38);
	s = (void *)call(IntuitionBase, LVOOpenScreenTagList, "a0a1", 0, st);
	wt[1] = (u32)s;
	w = (void *)call(IntuitionBase, LVOOpenWindowTagList, "a0a1", 0, wt);
	at[1] = (u32)w;
	r = (u8 *)call(AslBase, -48, "d0a0", type, at);         /* AllocAslRequest */
	ok = call(AslBase, -60, "a0a1", r, 0);                  /* AslRequest */
	printf("AslRequest = %ld\n", ok);
	if (ok && type == 0)
		printf("drawer \"%s\" file \"%s\"\n", *(char **)(r + 8), *(char **)(r + 4));
	if (ok && type == 1)
		printf("font %s/%ld\n", *(char **)(r + 8), (u32)*(u16 *)(r + 12));
	if (ok && type == 2)
		printf("mode $%lx %ldx%ld depth %ld\n", *(u32 *)r, *(u32 *)(r + 4),
		       *(u32 *)(r + 8), (u32)*(u16 *)(r + 12));
	call(AslBase, -54, "a0", r);
	call(IntuitionBase, LVOCloseWindow, "a0", w);
	call(IntuitionBase, LVOCloseScreen, "a0", s);
	return 0;
}
