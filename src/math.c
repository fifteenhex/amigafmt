/*
 * The math libraries: mathieeedoubbas, mathieeedoubtrans,
 * mathieeesingbas, mathieeesingtrans, mathffp, mathtrans.
 *
 * Doubles travel in two registers, high half first (d0/d1, d2/d3), and
 * come back in d0/d1; singles and FFP numbers in one.  Argument order is
 * the .fd's: for FFP that puts the left operand in d1 and the right one
 * in d0, and the operation is left OP right.  Fix truncates toward zero
 * and saturates.
 */
#include <math.h>
#include <string.h>
#include "af.h"

static double getd(struct frame *f, int r)
{
	uint64_t b = (uint64_t)f->d[r] << 32 | f->d[r + 1];
	double d;

	memcpy(&d, &b, 8);
	return d;
}

static void retd(struct frame *f, double d)
{
	uint64_t b;

	memcpy(&b, &d, 8);
	f->d[0] = b >> 32;
	f->d[1] = (uint32_t)b;
}

static float gets(uint32_t v)
{
	float x;

	memcpy(&x, &v, 4);
	return x;
}

static uint32_t sbits(float x)
{
	uint32_t v;

	memcpy(&v, &x, 4);
	return v;
}

static int32_t fix(double d)
{
	if (d != d)
		return 0;
	if (d >= 2147483647.0)
		return 0x7fffffff;
	if (d <= -2147483648.0)
		return (int32_t)0x80000000;
	return (int32_t)d;
}

static int32_t cmp(double a, double b)
{
	return a > b ? 1 : a < b ? -1 : 0;
}

/* ---- FFP: mantissa in bits 31..8 (bit 31 set), sign bit 7, exponent
   bits 6..0 excess 64, value = mantissa/2^24 * 2^(exp-64) ---- */

static double ffp2d(uint32_t v)
{
	double m = (double)(v >> 8) / 16777216.0;
	int e = (int)(v & 0x7f) - 64;

	if (!(v & 0xffffff00))
		return 0.0;
	m = ldexp(m, e);
	return v & 0x80 ? -m : m;
}

static uint32_t d2ffp(double d)
{
	int e, neg = d < 0;
	double m;
	uint32_t mant;

	if (d == 0.0 || d != d)
		return 0;
	m = frexp(neg ? -d : d, &e);            /* 0.5 <= m < 1 */
	mant = (uint32_t)(m * 16777216.0 + 0.5);
	if (mant >= 0x1000000) {
		mant >>= 1;
		e++;
	}
	if (e + 64 > 127)
		return 0xffffff7f | (neg ? 0x80 : 0);  /* largest */
	if (e + 64 < 0)
		return 0;
	return mant << 8 | (neg ? 0x80 : 0) | (e + 64);
}

/* ---- mathieeedoubbas ---- */

static void dp_Fix(struct frame *f) { RET(fix(getd(f, 0))); }
static void dp_Flt(struct frame *f) { retd(f, (int32_t)D(0)); }
static void dp_Cmp(struct frame *f) { RET(cmp(getd(f, 0), getd(f, 2))); }
static void dp_Tst(struct frame *f) { RET(cmp(getd(f, 0), 0.0)); }
static void dp_Abs(struct frame *f) { retd(f, fabs(getd(f, 0))); }
static void dp_Neg(struct frame *f) { retd(f, -getd(f, 0)); }
static void dp_Add(struct frame *f) { retd(f, getd(f, 0) + getd(f, 2)); }
static void dp_Sub(struct frame *f) { retd(f, getd(f, 0) - getd(f, 2)); }
static void dp_Mul(struct frame *f) { retd(f, getd(f, 0) * getd(f, 2)); }
static void dp_Div(struct frame *f) { retd(f, getd(f, 0) / getd(f, 2)); }
static void dp_Floor(struct frame *f) { retd(f, floor(getd(f, 0))); }
static void dp_Ceil(struct frame *f) { retd(f, ceil(getd(f, 0))); }

static const struct libimpl dpbas_impl[] = {
	{ "IEEEDPFix", dp_Fix }, { "IEEEDPFlt", dp_Flt },
	{ "IEEEDPCmp", dp_Cmp }, { "IEEEDPTst", dp_Tst },
	{ "IEEEDPAbs", dp_Abs }, { "IEEEDPNeg", dp_Neg },
	{ "IEEEDPAdd", dp_Add }, { "IEEEDPSub", dp_Sub },
	{ "IEEEDPMul", dp_Mul }, { "IEEEDPDiv", dp_Div },
	{ "IEEEDPFloor", dp_Floor }, { "IEEEDPCeil", dp_Ceil },
	{ NULL, NULL }
};

/* ---- mathieeedoubtrans ---- */

#define DP1(name, fn) \
	static void name(struct frame *f) { retd(f, fn(getd(f, 0))); }
DP1(dp_Atan, atan) DP1(dp_Sin, sin) DP1(dp_Cos, cos) DP1(dp_Tan, tan)
DP1(dp_Sinh, sinh) DP1(dp_Cosh, cosh) DP1(dp_Tanh, tanh) DP1(dp_Exp, exp)
DP1(dp_Log, log) DP1(dp_Sqrt, sqrt) DP1(dp_Asin, asin) DP1(dp_Acos, acos)
DP1(dp_Log10, log10)

static void dp_Sincos(struct frame *f)
{
	double x = getd(f, 0), c = cos(x);
	uint64_t b;

	memcpy(&b, &c, 8);
	((uint32_t *)PTR(A(0)))[0] = b >> 32;
	((uint32_t *)PTR(A(0)))[1] = (uint32_t)b;
	retd(f, sin(x));
}

/* IEEEDPPow(exp, arg)(d2/d3, d0/d1) = arg ^ exp */
static void dp_Pow(struct frame *f) { retd(f, pow(getd(f, 0), getd(f, 2))); }
static void dp_Tieee(struct frame *f) { RET(sbits((float)getd(f, 0))); }
static void dp_Fieee(struct frame *f) { retd(f, gets(D(0))); }

static const struct libimpl dptrans_impl[] = {
	{ "IEEEDPAtan", dp_Atan }, { "IEEEDPSin", dp_Sin },
	{ "IEEEDPCos", dp_Cos }, { "IEEEDPTan", dp_Tan },
	{ "IEEEDPSincos", dp_Sincos }, { "IEEEDPSinh", dp_Sinh },
	{ "IEEEDPCosh", dp_Cosh }, { "IEEEDPTanh", dp_Tanh },
	{ "IEEEDPExp", dp_Exp }, { "IEEEDPLog", dp_Log },
	{ "IEEEDPPow", dp_Pow }, { "IEEEDPSqrt", dp_Sqrt },
	{ "IEEEDPTieee", dp_Tieee }, { "IEEEDPFieee", dp_Fieee },
	{ "IEEEDPAsin", dp_Asin }, { "IEEEDPAcos", dp_Acos },
	{ "IEEEDPLog10", dp_Log10 },
	{ NULL, NULL }
};

/* ---- mathieeesingbas / mathieeesingtrans ---- */

static void sp_Fix(struct frame *f) { RET(fix(gets(D(0)))); }
static void sp_Flt(struct frame *f) { RET(sbits((float)(int32_t)D(0))); }
static void sp_Cmp(struct frame *f) { RET(cmp(gets(D(0)), gets(D(1)))); }
static void sp_Tst(struct frame *f) { RET(cmp(gets(D(0)), 0.0)); }
static void sp_Abs(struct frame *f) { RET(sbits(fabsf(gets(D(0))))); }
static void sp_Neg(struct frame *f) { RET(sbits(-gets(D(0)))); }
static void sp_Add(struct frame *f) { RET(sbits(gets(D(0)) + gets(D(1)))); }
static void sp_Sub(struct frame *f) { RET(sbits(gets(D(0)) - gets(D(1)))); }
static void sp_Mul(struct frame *f) { RET(sbits(gets(D(0)) * gets(D(1)))); }
static void sp_Div(struct frame *f) { RET(sbits(gets(D(0)) / gets(D(1)))); }
static void sp_Floor(struct frame *f) { RET(sbits(floorf(gets(D(0))))); }
static void sp_Ceil(struct frame *f) { RET(sbits(ceilf(gets(D(0))))); }

static const struct libimpl spbas_impl[] = {
	{ "IEEESPFix", sp_Fix }, { "IEEESPFlt", sp_Flt },
	{ "IEEESPCmp", sp_Cmp }, { "IEEESPTst", sp_Tst },
	{ "IEEESPAbs", sp_Abs }, { "IEEESPNeg", sp_Neg },
	{ "IEEESPAdd", sp_Add }, { "IEEESPSub", sp_Sub },
	{ "IEEESPMul", sp_Mul }, { "IEEESPDiv", sp_Div },
	{ "IEEESPFloor", sp_Floor }, { "IEEESPCeil", sp_Ceil },
	{ NULL, NULL }
};

#define SP1(name, fn) \
	static void name(struct frame *f) { RET(sbits(fn(gets(D(0))))); }
SP1(sp_Atan, atanf) SP1(sp_Sin, sinf) SP1(sp_Cos, cosf) SP1(sp_Tan, tanf)
SP1(sp_Sinh, sinhf) SP1(sp_Cosh, coshf) SP1(sp_Tanh, tanhf) SP1(sp_Exp, expf)
SP1(sp_Log, logf) SP1(sp_Sqrt, sqrtf) SP1(sp_Asin, asinf) SP1(sp_Acos, acosf)
SP1(sp_Log10, log10f)

static void sp_Sincos(struct frame *f)
{
	float x = gets(D(0));

	*(uint32_t *)PTR(A(0)) = sbits(cosf(x));
	RET(sbits(sinf(x)));
}

/* IEEESPPow(exp, arg)(d1, d0) = arg ^ exp */
static void sp_Pow(struct frame *f) { RET(sbits(powf(gets(D(0)), gets(D(1))))); }
static void sp_Same(struct frame *f) { RET(D(0)); }

static const struct libimpl sptrans_impl[] = {
	{ "IEEESPAtan", sp_Atan }, { "IEEESPSin", sp_Sin },
	{ "IEEESPCos", sp_Cos }, { "IEEESPTan", sp_Tan },
	{ "IEEESPSincos", sp_Sincos }, { "IEEESPSinh", sp_Sinh },
	{ "IEEESPCosh", sp_Cosh }, { "IEEESPTanh", sp_Tanh },
	{ "IEEESPExp", sp_Exp }, { "IEEESPLog", sp_Log },
	{ "IEEESPPow", sp_Pow }, { "IEEESPSqrt", sp_Sqrt },
	{ "IEEESPTieee", sp_Same }, { "IEEESPFieee", sp_Same },
	{ "IEEESPAsin", sp_Asin }, { "IEEESPAcos", sp_Acos },
	{ "IEEESPLog10", sp_Log10 },
	{ NULL, NULL }
};

/* ---- mathffp / mathtrans ---- */

#define F(x) ffp2d(x)
static void ffp_Fix(struct frame *f) { RET(fix(F(D(0)))); }
static void ffp_Flt(struct frame *f) { RET(d2ffp((int32_t)D(0))); }
static void ffp_Cmp(struct frame *f) { RET(cmp(F(D(1)), F(D(0)))); }
static void ffp_Tst(struct frame *f) { RET(cmp(F(D(1)), 0.0)); }
static void ffp_Abs(struct frame *f) { RET(D(0) & ~0x80u); }
static void ffp_Neg(struct frame *f) { RET(D(0) & 0xffffff00 ? D(0) ^ 0x80 : 0); }
static void ffp_Add(struct frame *f) { RET(d2ffp(F(D(1)) + F(D(0)))); }
static void ffp_Sub(struct frame *f) { RET(d2ffp(F(D(1)) - F(D(0)))); }
static void ffp_Mul(struct frame *f) { RET(d2ffp(F(D(1)) * F(D(0)))); }
static void ffp_Div(struct frame *f) { RET(d2ffp(F(D(1)) / F(D(0)))); }
static void ffp_Floor(struct frame *f) { RET(d2ffp(floor(F(D(0))))); }
static void ffp_Ceil(struct frame *f) { RET(d2ffp(ceil(F(D(0))))); }

static const struct libimpl ffp_impl[] = {
	{ "SPFix", ffp_Fix }, { "SPFlt", ffp_Flt }, { "SPCmp", ffp_Cmp },
	{ "SPTst", ffp_Tst }, { "SPAbs", ffp_Abs }, { "SPNeg", ffp_Neg },
	{ "SPAdd", ffp_Add }, { "SPSub", ffp_Sub }, { "SPMul", ffp_Mul },
	{ "SPDiv", ffp_Div }, { "SPFloor", ffp_Floor }, { "SPCeil", ffp_Ceil },
	{ NULL, NULL }
};

#define FT1(name, fn) \
	static void name(struct frame *f) { RET(d2ffp(fn(F(D(0))))); }
FT1(ft_Atan, atan) FT1(ft_Sin, sin) FT1(ft_Cos, cos) FT1(ft_Tan, tan)
FT1(ft_Sinh, sinh) FT1(ft_Cosh, cosh) FT1(ft_Tanh, tanh) FT1(ft_Exp, exp)
FT1(ft_Log, log) FT1(ft_Sqrt, sqrt) FT1(ft_Asin, asin) FT1(ft_Acos, acos)
FT1(ft_Log10, log10)

/* SPSincos(cosResult, parm)(d1, d0): d1 points at the cosine */
static void ft_Sincos(struct frame *f)
{
	double x = F(D(0));

	*(uint32_t *)PTR(D(1)) = d2ffp(cos(x));
	RET(d2ffp(sin(x)));
}

/* SPPow(power, arg)(d1, d0) = arg ^ power */
static void ft_Pow(struct frame *f) { RET(d2ffp(pow(F(D(0)), F(D(1))))); }
static void ft_Tieee(struct frame *f) { RET(sbits((float)F(D(0)))); }
static void ft_Fieee(struct frame *f) { RET(d2ffp(gets(D(0)))); }

static const struct libimpl mtrans_impl[] = {
	{ "SPAtan", ft_Atan }, { "SPSin", ft_Sin }, { "SPCos", ft_Cos },
	{ "SPTan", ft_Tan }, { "SPSincos", ft_Sincos }, { "SPSinh", ft_Sinh },
	{ "SPCosh", ft_Cosh }, { "SPTanh", ft_Tanh }, { "SPExp", ft_Exp },
	{ "SPLog", ft_Log }, { "SPPow", ft_Pow }, { "SPSqrt", ft_Sqrt },
	{ "SPTieee", ft_Tieee }, { "SPFieee", ft_Fieee }, { "SPAsin", ft_Asin },
	{ "SPAcos", ft_Acos }, { "SPLog10", ft_Log10 },
	{ NULL, NULL }
};

struct amilib lib_mathieeedoubbas = {
	.name = "mathieeedoubbas.library", .version = 40, .revision = 4,
	.fd = fd_mathieeedoubbas, .impl = dpbas_impl, .possize = 8,
};
struct amilib lib_mathieeedoubtrans = {
	.name = "mathieeedoubtrans.library", .version = 40, .revision = 4,
	.fd = fd_mathieeedoubtrans, .impl = dptrans_impl, .possize = 8,
};
struct amilib lib_mathieeesingbas = {
	.name = "mathieeesingbas.library", .version = 40, .revision = 4,
	.fd = fd_mathieeesingbas, .impl = spbas_impl, .possize = 8,
};
struct amilib lib_mathieeesingtrans = {
	.name = "mathieeesingtrans.library", .version = 40, .revision = 4,
	.fd = fd_mathieeesingtrans, .impl = sptrans_impl, .possize = 8,
};
struct amilib lib_mathffp = {
	.name = "mathffp.library", .version = 40, .revision = 1,
	.fd = fd_mathffp, .impl = ffp_impl, .possize = 8,
};
struct amilib lib_mathtrans = {
	.name = "mathtrans.library", .version = 40, .revision = 1,
	.fd = fd_mathtrans, .impl = mtrans_impl, .possize = 8,
};
