/* exec memory, lists, signals, libraries; utility arithmetic */
#include "t.h"

struct Node { struct Node *succ, *pred; u8 type; s8 pri; char *name; };
struct List { struct Node *head, *tail, *tailpred; u8 type, pad; };

int main(u32 arglen, char *args)
{
	struct List l;
	struct Node n[3] = { { 0, 0, 0, 0, "one" }, { 0, 0, 0, 5, "two" },
			     { 0, 0, 0, -5, "three" } };
	struct Node *x;
	void *ub, *m;
	struct R r;
	u32 *v;

	l.head = (struct Node *)&l.tail;
	l.tail = 0;
	l.tailpred = (struct Node *)&l.head;
	call(SysBase, LVOAddTail, "a0a1", &l, &n[0]);
	call(SysBase, LVOEnqueue, "a0a1", &l, &n[1]);
	call(SysBase, LVOEnqueue, "a0a1", &l, &n[2]);
	for (x = l.head; x->succ; x = x->succ)
		printf("node %s pri %ld\n", x->name, (s32)x->pri);
	x = (struct Node *)call(SysBase, LVOFindName, "a0a1", &l, "three");
	printf("FindName: %s\n", x ? x->name : "(none)");
	x = (struct Node *)call(SysBase, LVORemHead, "a0", &l);
	printf("RemHead: %s, next %s\n", x->name, l.head->name);

	m = (void *)call(SysBase, LVOAllocMem, "d0d1", 1000, 0x10001);
	printf("AllocMem aligned %ld cleared %ld\n", ((u32)m & 7) == 0,
	       ((u32 *)m)[100] == 0);
	call(SysBase, LVOCopyMem, "a0a1d0", "copied", m, 7);
	printf("CopyMem: %s\n", (char *)m);
	call(SysBase, LVOFreeMem, "a1d0", m, 1000);
	v = (u32 *)call(SysBase, LVOAllocVec, "d0d1", 64, 0x10000);
	printf("AllocVec %s\n", v ? "ok" : "failed");
	call(SysBase, LVOFreeVec, "a1", v);

	printf("FindTask(0) is ThisTask: %ld\n",
	       call(SysBase, LVOFindTask, "a1", 0) ==
	       *(u32 *)((char *)SysBase + 276));
	call(SysBase, LVOSetSignal, "d0d1", 1 << 12, 1 << 12);
	printf("SetSignal old has ^C: %ld\n",
	       (call(SysBase, LVOSetSignal, "d0d1", 0, 1 << 12) >> 12) & 1);
	printf("now: %ld\n",
	       (call(SysBase, LVOSetSignal, "d0d1", 0, 0) >> 12) & 1);

	ub = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "utility.library", 37);
	printf("utility %s\n", ub ? "opened" : "missing");
	printf("UMult32 %ld\n", call(ub, LVOUMult32, "d0d1", 1234, 5678));
	memset(&r, 0, sizeof(r));
	r.d[0] = (u32)-100;
	r.d[1] = 7;
	amicall(&r, ub, LVOSDivMod32);
	printf("SDivMod32 %ld rem %ld\n", r.d[0], r.d[1]);
	r.d[0] = 0x80000000;
	r.d[1] = 4;
	amicall(&r, ub, LVOUMult64);
	printf("UMult64 lo %ld hi %ld\n", r.d[0], r.d[1]);
	printf("Stricmp %ld %ld\n", call(ub, LVOStricmp, "a0a1", "Hello", "hELLO"),
	       call(ub, LVOStricmp, "a0a1", "abc", "abd"));
	printf("missing library: %ld\n",
	       call(SysBase, LVOOpenLibrary, "a1d0", "nonesuch.library", 0));
	printf("dos too new: %ld\n",
	       call(SysBase, LVOOpenLibrary, "a1d0", "dos.library", 99));
	call(SysBase, LVOCloseLibrary, "a1", ub);
	return 5;
}
