/* RawDoFmt and VPrintf formatting */
#include "t.h"

static char buf[256];

/* PutChProc: move.b d0,(a3)+ ; rts */
static const u16 putch[] = { 0x16c0, 0x4e75 };
/* a PutChProc amigafmt cannot shortcut: uppercase into the buffer */
static const u16 putch_up[] = {
	0x0c00, 0x0061,		/* cmp.b #'a',d0 */
	0x6d0a,			/* blt.s store */
	0x0c00, 0x007a,		/* cmp.b #'z',d0 */
	0x6e04,			/* bgt.s store */
	0x0400, 0x0020,		/* sub.b #32,d0 */
	0x16c0, 0x4e75		/* store: move.b d0,(a3)+ ; rts */
};

static void fmt(const u16 *pc, const char *f, void *data)
{
	call(SysBase, LVORawDoFmt, "a0a1a2a3", f, data, pc, buf);
	printf("[%s]\n", buf);
}

int main(u32 arglen, char *args)
{
	struct { u16 w; s32 l; char *s; u16 c; } __attribute__((packed)) d1 =
		{ 0xfffe, 123456, "str", 'X' };
	u32 d2[] = { 0xdeadbeef, 42, (u32)"abcdef", (u32)-7 };

	fmt(putch, "w=%d l=%ld s=%s c=%c", &d1);
	fmt(putch, "%08lx|%-6ld|%.3s|%%|%ld", d2);
	fmt(putch, "%lu %lx", d2);
	fmt(putch_up, "upper %s", &d2[2]);
	printf("printf %ld %s %5ld|%-5ld|\n", 1, "two", 3, 4);
	printf("neg %ld %05ld\n", -12, -12);
	return 0;
}
