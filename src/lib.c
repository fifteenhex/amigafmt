/*
 * Library bases with real jump tables.
 *
 * Every vector is a 6-byte "jmp stub" at a negative offset from the base,
 * as AmigaOS has it, so `jsr -offset(a6)`, SetFunction() and programs
 * that read the vector all behave.  Each stub pushes the library and slot
 * number and enters lvo_common (glue.S), which calls amiga_dispatch().
 * Implementations are bound to slots BY NAME through the tables
 * tools/fd2c.py generates from the official .fd files.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "af.h"



extern void lvo_common(void);

struct amilib *all_libs[] = { &lib_exec, &lib_dos, &lib_utility,
			      &lib_version, &lib_intuition, &lib_graphics,
			      &lib_gadtools, &lib_mathieeedoubbas,
			      &lib_mathieeedoubtrans, &lib_mathieeesingbas,
			      &lib_mathieeesingtrans, &lib_mathffp,
			      &lib_mathtrans, &lib_timer, &lib_icon, &lib_layers, &lib_console, &lib_asl,
			      &lib_iffparse, &lib_boopsi, &lib_workbench,
			      &lib_datatypes,
			      NULL };
struct frame *resume_frame;
int dispatch_all_regs;

/* the four vectors every library has, below the .fd bias of 30 */
static void l_open(struct frame *f)
{
	struct Library *b = PTR(A(6));
	b->lib_OpenCnt++;
	RET(b);
}

static void l_close(struct frame *f)
{
	struct Library *b = PTR(A(6));
	if (b->lib_OpenCnt)
		b->lib_OpenCnt--;
	RET(0);
}

static void l_null(struct frame *f)
{
	RET(0);
}

static const char *std_names[] = { "Open", "Close", "Expunge", "Reserved" };

const char *lib_fn_name(struct amilib *l, int slot, const char **regs)
{
	const struct fdent *e;

	*regs = "";
	if (slot >= 1 && slot <= 4)
		return std_names[slot - 1];
	for (e = l->fd; e && e->name; e++)
		if (e->bias == slot * 6) {
			*regs = e->regs;
			return e->name;
		}
	return "?";
}

static int fd_slots(const struct fdent *fd)
{
	int n = 4;

	for (; fd && fd->name; fd++)
		if (fd->bias / 6 > n)
			n = fd->bias / 6;
	return n;
}

static void bind(struct amilib *l, const struct libimpl *im)
{
	for (; im && im->name; im++) {
		const struct fdent *e;
		for (e = l->fd; e && e->name; e++)
			if (!strcmp(e->name, im->name))
				break;
		if (!e || !e->name)
			af_die("internal: %s has no vector %s", l->name, im->name);
		l->table[e->bias / 6] = im->fn;
	}
}

struct Library *lib_build(struct amilib *l)
{
	uint32_t neg, *stubs;
	uint16_t *jt;
	char *mem;
	struct Library *b;
	int i;

	if (l->base)
		return l->base;
	l->nslots = fd_slots(l->fd);
	if (l->nslots < l->minslots)
		l->nslots = l->minslots;
	/* spare vectors past the .fd, so a call into a newer OS's part of
	   the table is reported instead of running into whatever is below */
	l->nslots += 32;
	l->table = calloc(l->nslots + 1, sizeof(lvo_fn));
	for (i = 0; all_libs[i]; i++)
		if (all_libs[i] == l)
			l->index = i;
	l->table[1] = l_open;
	l->table[2] = l_close;
	l->table[3] = l_null;
	l->table[4] = l_null;
	bind(l, l->impl);
	bind(l, l->impl2);

	neg = (l->nslots * 6 + 3) & ~3u;
	mem = amem_alloc(neg + sizeof(struct Library) + l->possize
			 + l->nslots * 12 + 64);
	b = (struct Library *)(mem + neg);
	stubs = (uint32_t *)((char *)b + ((sizeof(struct Library) + l->possize
					   + 3) & ~3u));
	for (i = 1; i <= l->nslots; i++) {
		uint16_t *s = (uint16_t *)((char *)stubs + (i - 1) * 12);
		uint32_t id = (uint32_t)l->index << 16 | i;
		uint32_t tgt = (uint32_t)(uintptr_t)lvo_common;

		s[0] = 0x2f3c;                  /* move.l #id,-(sp) */
		s[1] = id >> 16;
		s[2] = id;
		s[3] = 0x4ef9;                  /* jmp lvo_common */
		s[4] = tgt >> 16;
		s[5] = tgt;
		jt = (uint16_t *)((char *)b - i * 6);
		tgt = (uint32_t)(uintptr_t)s;
		jt[0] = 0x4ef9;                 /* jmp stub */
		jt[1] = tgt >> 16;
		jt[2] = tgt;
	}
	cache_flush(mem, neg + sizeof(struct Library) + l->possize
		    + l->nslots * 12 + 64);

	b->lib_Node.ln_Type = NT_LIBRARY;
	b->lib_Node.ln_Name = APTR_OF(l->name);
	b->lib_Flags = 6;                       /* LIBF_CHANGED|LIBF_SUMUSED */
	b->lib_NegSize = neg;
	b->lib_PosSize = sizeof(struct Library) + l->possize;
	b->lib_Version = l->version;
	b->lib_Revision = l->revision;
	{
		char *id = malloc(64);
		snprintf(id, 64, "%s %u.%u (amigafmt)\r\n", l->name,
			 l->version, l->revision);
		b->lib_IdString = APTR_OF(id);
	}
	l->base = b;
	if (l->init)
		l->init(b);
	return b;
}

struct amilib *lib_find(const char *name)
{
	const char *p = strrchr(name, '/'), *q = strrchr(name, ':');
	int i;

	if (q && (!p || q > p))
		p = q;
	p = p ? p + 1 : name;
	for (i = 0; all_libs[i]; i++)
		if (!strcasecmp(all_libs[i]->name, p) && !all_libs[i]->device)
			return all_libs[i];
	return NULL;
}

/* can [addr, addr+len) be read?  A pipe makes the kernel tell us. */
static int readable(uint32_t addr, uint32_t len)
{
	static int p[2] = { -1, -1 };
	char buf[64];

	if (addr < 0x1000 || len > sizeof(buf))
		return 0;
	if (p[0] < 0 && pipe(p))
		return 0;
	if (write(p[1], PTR(addr), len) != (ssize_t)len)
		return 0;
	return read(p[0], buf, len) == (ssize_t)len;
}

static void trace_call(struct amilib *l, int slot, struct frame *f, int after)
{
	const char *regs, *name = lib_fn_name(l, slot, &regs);

	if (after) {
		fprintf(stderr, " = $%x\n", f->d[0]);
		return;
	}
	fprintf(stderr, "[%s] %s(", l->name, name);
	for (const char *r = regs; *r; ) {
		int n = r[1] - '0';
		uint32_t v = r[0] == 'd' ? f->d[n] : f->a[n];
		fprintf(stderr, "%c%c=$%x", r[0], r[1], v);
		if (readable(v, 41)) {
			/* show something printable behind pointers */
			const unsigned char *s = PTR(v);
			int ok = 1, k;
			for (k = 0; k < 40 && s[k]; k++)
				if (s[k] < 32 || s[k] > 126)
					ok = 0;
			if (ok && k > 0 && k < 40)
				fprintf(stderr, "\"%s\"", s);
		}
		r += 2;
		if (*r == '/')
			r++, fputc(',', stderr);
	}
	fputc(')', stderr);
}

static void unimplemented(struct amilib *l, int slot, struct frame *f)
{
	const char *regs, *name = lib_fn_name(l, slot, &regs);

	dos_flush_all();
	fprintf(stderr, "amigafmt: %s: unimplemented %s -%d (%s) called from "
		"$%08x; stopping\n", af_progname, l->name, slot * 6, name,
		f->ret - 6);
	exit(20);
}

struct frame *amiga_dispatch(struct frame *f)
{
	unsigned li = f->id >> 16, slot = f->id & 0xffff;
	struct amilib *l = all_libs[li];
	lvo_fn fn = l->table[slot];
	struct frame *outer = resume_frame, *r;

	if (!fn)
		unimplemented(l, slot, f);
	uint32_t keep[11], fpcr;

	resume_frame = f;
	if (af_trace)
		trace_call(l, slot, f, 0);
	/* d2-d7/a2-a6 survive a library call whatever the handler did with
	   its copy (SAS/C relies on it: C:Status reuses d2 across VPrintf) */
	memcpy(keep, &f->d[2], 6 * 4);
	memcpy(keep + 6, &f->a[2], 5 * 4);
	exec_pending_break();
	dispatch_all_regs = 0;
	/* the program's FPU control register (SAS/C sets single precision)
	   must not leak into our libm: C runs with the default, and the
	   program gets its own back */
	__asm__ volatile("fmove.l %%fpcr,%0" : "=d"(fpcr));
	if (fpcr)
		__asm__ volatile("fmove.l %0,%%fpcr" : : "d"(0));
	fn(f);
	if (fpcr)
		__asm__ volatile("fmove.l %0,%%fpcr" : : "d"(fpcr));
	if (!dispatch_all_regs) {
		memcpy(&f->d[2], keep, 6 * 4);
		memcpy(&f->a[2], keep + 6, 5 * 4);
	}
	dispatch_all_regs = 0;
	if (af_trace)
		trace_call(l, slot, f, 1);
	exec_pending_break();
	r = resume_frame;
	resume_frame = outer;
	return r;
}

uint32_t amiga_call_super(struct frame *f, uint32_t pc, struct regs *r)
{
	r->a[7] = ((uint32_t)(uintptr_t)f - 64) & ~3u;
	return call_amiga_super(r, pc);
}

/* call Amiga code from inside a handler, on the Amiga stack below f */
uint32_t amiga_call(struct frame *f, uint32_t pc, struct regs *r)
{
	r->a[7] = ((uint32_t)(uintptr_t)f - 64) & ~3u;
	return call_amiga(r, pc);
}

void af_die(const char *fmt, ...)
{
	va_list ap;

	dos_flush_all();
	fprintf(stderr, "amigafmt: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(20);
}

void af_warn(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "amigafmt: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
