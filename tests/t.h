/* amigafmt test programs: a tiny freestanding AmigaOS runtime */
#include <stdarg.h>

typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef short s16;
typedef signed char s8;
typedef unsigned char u8;

struct R { u32 d[8]; u32 a[6]; };
u32 amicall(struct R *r, void *base, int off);
extern void *SysBase;
extern void *DOSBase;

/* call base+off with arguments in the named registers, e.g. "d1d2d3" */
u32 call(void *base, int off, const char *regs, ...);
void printf(const char *fmt, ...);      /* dos VPrintf: %ld etc. */
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
int strcmp(const char *a, const char *b);
unsigned long strlen(const char *s);

/* vector offsets, generated from the NDK .fd files by tools/fd2c.py --lvo */
#include "lvo.h"
