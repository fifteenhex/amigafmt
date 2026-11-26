#include "t.h"

void *DOSBase;

u32 call(void *base, int off, const char *regs, ...)
{
	struct R r;
	va_list ap;

	memset(&r, 0, sizeof(r));
	va_start(ap, regs);
	for (; *regs; regs += 2) {
		u32 v = va_arg(ap, u32);
		if (regs[0] == 'd')
			r.d[regs[1] - '0'] = v;
		else
			r.a[regs[1] - '0'] = v;
	}
	va_end(ap);
	return amicall(&r, base, off);
}

void printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	call(DOSBase, LVOVPrintf, "d1d2", fmt, ap);
	va_end(ap);
}

void *memset(void *d, int c, unsigned long n)
{
	u8 *p = d;
	while (n--)
		*p++ = c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	u8 *p = d;
	const u8 *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (u8)*a - (u8)*b;
}

unsigned long strlen(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

int main(u32 arglen, char *args);

int tmain(u32 arglen, char *args)
{
	int rc;

	DOSBase = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "dos.library", 37);
	if (!DOSBase)
		return 20;
	rc = main(arglen, args);
	call(SysBase, LVOCloseLibrary, "a1", DOSBase);
	return rc;
}
