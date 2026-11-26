/* the machine: expansion.library's boards, exec's memory list and
   resources, a task trap handler, GfxBase's chip revision, custom chip
   reads; run with AMIGAFMT_ZORRO set to three boards */
#include "t.h"

/* expansion.library (not in lvo.h) */
#define LVOFindConfigDev (-72)
#define LVOAllocConfigDev (-48)
#define LVOFreeConfigDev (-84)
#define LVOAddConfigDev (-30)
#define LVORemConfigDev (-108)
#define LVOReadExpansionRom (-102)
#define LVOAllocExpansionMem (-54)

struct Node { struct Node *succ, *pred; u8 type; s8 pri; char *name; };
struct ConfigDev {
	struct Node node;
	u8 flags, pad;
	u8 er_Type, er_Product, er_Flags, er_Res;
	u16 er_Manufacturer;
	u32 er_Serial;
	u16 er_Diag;
	u8 er_res[4];
	u32 addr, size;
};
struct MemHeader {
	struct Node node;
	u16 attr;
	u32 first, lower, upper, free;
} __attribute__((packed));

/* the handler takes the trap number and the frame off the stack and
   carries on after the instruction; not with RTE, which some versions of
   qemu-m68k abort on rather than raising SIGILL */
u32 trapnum, trapvec;
void trap_handler(void);
__asm__(".globl trap_handler\n"
	"trap_handler:\n"
	"	move.l (%sp)+,trapnum\n"
	"	move.w 6(%sp),trapvec+2\n"
	"	move.l 2(%sp),-(%sp)\n"
	"	addq.l #4,(%sp)\n"
	"	move.l (%sp)+,4(%sp)\n"
	"	addq.l #4,%sp\n"
	"	rts\n");

static u32 lineF(void)
{
	u32 v = 1;
	__asm__ volatile("subq.l #4,%%sp\n\t.word 0xf017,0x4200\n\t"
			 "addq.l #4,%%sp\n\tmoveq #2,%0" : "=d"(v) : : "memory");
	return v;
}

int main(u32 arglen, char *args)
{
	u8 *sys = SysBase, *gfx;
	void *eb = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "expansion.library", 37);
	struct ConfigDev *cd = 0, *mine;
	struct MemHeader *mh;
	u8 *task;
	u16 vposr;
	struct Node res = { 0, 0, 0, 0, "test.resource" };

	if (!eb)
		return 20;
	while ((cd = (void *)call(eb, LVOFindConfigDev, "a0d0d1", cd, -1, -1)))
		printf("board %ld/%ld type $%lx flags $%lx at $%lx size $%lx\n",
		       (u32)cd->er_Manufacturer, (u32)cd->er_Product,
		       (u32)cd->er_Type, (u32)cd->er_Flags, cd->addr, cd->size);
	cd = (void *)call(eb, LVOFindConfigDev, "a0d0d1", 0, 2206, -1);
	cd = cd ? (void *)call(eb, LVOFindConfigDev, "a0d0d1", cd, 2206, -1) : 0;
	printf("second 2206 board: product %ld\n", cd ? (u32)cd->er_Product : 0);
	printf("1/1: %ld\n", call(eb, LVOFindConfigDev, "a0d0d1", 0, 1, 1));
	mine = (void *)call(eb, LVOAllocConfigDev, "");
	mine->er_Manufacturer = 1;
	mine->er_Product = 2;
	call(eb, LVOAddConfigDev, "a0", mine);
	printf("added: %ld\n", call(eb, LVOFindConfigDev, "a0d0d1", 0, 1, 2) == (u32)mine);
	call(eb, LVORemConfigDev, "a0", mine);
	call(eb, LVOFreeConfigDev, "a0", mine);
	printf("removed: %ld\n", call(eb, LVOFindConfigDev, "a0d0d1", 0, 1, 2));
	printf("ReadExpansionRom: %ld, AllocExpansionMem: %ld\n",
	       call(eb, LVOReadExpansionRom, "a0a1", 0xe80000, 0) != 0,
	       call(eb, LVOAllocExpansionMem, "d0d1", 1, 0));
	call(SysBase, LVOCloseLibrary, "a1", eb);

	/* SysBase->MemList at 322 */
	for (mh = *(struct MemHeader **)(sys + 322); mh->node.succ;
	     mh = (struct MemHeader *)mh->node.succ) {
		printf("memory \"%s\" type %ld pri %ld attributes $%lx", mh->node.name,
		       (u32)mh->node.type, (s32)mh->node.pri, (u32)mh->attr);
		if (mh->attr & 2)
			printf(" size $%lx free $%lx", mh->upper - (u32)mh,
			       mh->free);
		printf(" lower-header %ld\n", mh->lower - (u32)mh);
	}
	printf("AvailMem(CHIP|TOTAL) $%lx\n",
	       call(SysBase, LVOAvailMem, "d1", (1 << 19) | 2));
	printf("VBlankFrequency %ld, MaxLocMem $%lx\n", (u32)sys[530],
	       *(u32 *)(sys + 62));

	call(SysBase, LVOAddResource, "a1", &res);
	printf("OpenResource: %ld\n",
	       call(SysBase, LVOOpenResource, "a1", "test.resource") == (u32)&res);
	call(SysBase, LVORemResource, "a1", &res);
	printf("after RemResource: %ld\n",
	       call(SysBase, LVOOpenResource, "a1", "test.resource"));

	gfx = (u8 *)call(SysBase, LVOOpenLibrary, "a1d0", "graphics.library", 0);
	printf("ChipRevBits0 $%lx, DisplayFlags %ld\n", (u32)gfx[0xec],
	       (u32)*(u16 *)(gfx + 0xce));
	call(SysBase, LVOCloseLibrary, "a1", gfx);
	__asm__ volatile("move.w #0x7f00,%0\n\tand.w 0xdff004,%0" : "=d"(vposr));
	printf("VPOSR & $7f00: $%lx\n", (u32)vposr);

	/* tc_TrapCode at 50 */
	task = (u8 *)call(SysBase, LVOFindTask, "a1", 0);
	*(void **)(task + 50) = trap_handler;
	vposr = lineF();
	printf("line F: %ld, trap %ld, vector offset $%lx\n", (u32)vposr,
	       trapnum, trapvec);
	return 0;
}
