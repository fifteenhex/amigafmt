/* the math libraries: IEEE double and Motorola FFP */
#include "t.h"

static void *dbas, *dtrans, *ffp;

/* a double travels as two longs, high first */
static u32 hi, lo;

static void dcall(void *base, int off, u32 ah, u32 al, u32 bh, u32 bl)
{
	struct R r;

	memset(&r, 0, sizeof(r));
	r.d[0] = ah; r.d[1] = al; r.d[2] = bh; r.d[3] = bl;
	amicall(&r, base, off);
	hi = r.d[0];
	lo = r.d[1];
}

static s32 fix(void)
{
	return call(dbas, LVOIEEEDPFix, "d0d1", hi, lo);
}

static void flt(s32 v)
{
	dcall(dbas, LVOIEEEDPFlt, v, 0, 0, 0);
}

int main(u32 arglen, char *args)
{
	u32 ah, al, f1, f2;

	dbas = (void *)call(SysBase, LVOOpenLibrary, "a1d0",
			    "mathieeedoubbas.library", 34);
	dtrans = (void *)call(SysBase, LVOOpenLibrary, "a1d0",
			      "mathieeedoubtrans.library", 34);
	ffp = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "mathffp.library", 0);
	if (!dbas || !dtrans || !ffp)
		return 20;
	flt(3); ah = hi; al = lo;
	flt(4);
	dcall(dbas, LVOIEEEDPDiv, ah, al, hi, lo);      /* 0.75 */
	ah = hi; al = lo;
	flt(1000);
	dcall(dbas, LVOIEEEDPMul, ah, al, hi, lo);
	printf("3/4*1000 = %ld\n", fix());
	flt(2);
	dcall(dtrans, LVOIEEEDPSqrt, hi, lo, 0, 0);
	ah = hi; al = lo;
	flt(1000000);
	dcall(dbas, LVOIEEEDPMul, ah, al, hi, lo);
	printf("sqrt(2)*1e6 = %ld\n", fix());
	flt(-7); ah = hi; al = lo;
	flt(2);
	dcall(dbas, LVOIEEEDPDiv, ah, al, hi, lo);
	dcall(dbas, LVOIEEEDPFloor, hi, lo, 0, 0);
	printf("floor(-3.5) = %ld\n", fix());
	flt(5); ah = hi; al = lo; flt(6);
	f1 = call(dbas, LVOIEEEDPCmp, "d0d1d2d3", ah, al, hi, lo);
	flt(-1);
	f2 = call(dbas, LVOIEEEDPTst, "d0d1", hi, lo);
	printf("cmp(5,6) = %ld, tst(-1) = %ld\n", f1, f2);
	flt(2); ah = hi; al = lo; flt(10);
	dcall(dtrans, LVOIEEEDPPow, hi, lo, ah, al);    /* 10^2 */
	printf("pow(10,2) = %ld\n", fix());

	f1 = call(ffp, LVOSPFlt, "d0", 10);
	f2 = call(ffp, LVOSPFlt, "d0", 4);
	printf("ffp 10 = $%08lx\n", f1);
	f1 = call(ffp, LVOSPDiv, "d1d0", f1, f2);       /* 10 / 4 */
	f1 = call(ffp, LVOSPMul, "d1d0", f1, call(ffp, LVOSPFlt, "d0", 100));
	printf("ffp 10/4*100 = %ld\n", call(ffp, LVOSPFix, "d0", f1));
	return 0;
}
