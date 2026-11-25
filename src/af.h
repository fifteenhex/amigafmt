/* amigafmt: shared declarations */
#ifndef AF_H
#define AF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "amiga.h"

/* registers as call_amiga() takes and returns them */
struct regs {
	uint32_t d[8];
	uint32_t a[8];          /* a[7] = sp */
};

/* what lvo_common leaves on the Amiga stack */
struct frame {
	uint32_t d[8];
	uint32_t a[7];
	uint32_t id;            /* library << 16 | slot */
	uint32_t ret;           /* the caller's return address */
};

uint32_t call_amiga(struct regs *r, uint32_t pc);
extern uint32_t native_sp;

/* a library vector's implementation */
typedef void (*lvo_fn)(struct frame *f);

struct fdent {
	uint16_t bias;
	const char *name, *regs;
};

extern const struct fdent fd_exec[];

struct libimpl {
	const char *name;
	lvo_fn fn;
};

struct amilib {
	const char *name;
	uint16_t version, revision;
	const struct fdent *fd;
	const struct libimpl *impl;
	uint32_t possize;               /* bytes after the Library header */
	void (*init)(struct Library *base);
	int minslots;                   /* vectors when there is no .fd */
	int device;                     /* a device: OpenDevice, not OpenLibrary */
	/* filled in by lib_build() */
	struct Library *base;
	int nslots;
	lvo_fn *table;
	int index;
};

extern struct amilib lib_exec;
extern struct amilib *all_libs[];

struct Library *lib_build(struct amilib *l);
struct amilib *lib_find(const char *name);
const char *lib_fn_name(struct amilib *l, int slot, const char **regs);
struct frame *amiga_dispatch(struct frame *f);
extern struct frame *resume_frame;

/* globals */
extern struct ExecBase *SysBase;
extern struct Process *me;
extern int af_trace;
extern const char *af_progname;

/* memory that Amiga code may execute (qemu enforces PROT_EXEC) */
void *amem_alloc(uint32_t size);
void amem_free(void *p);
uint32_t amem_avail(void);
void cache_flush(void *p, uint32_t len);

/* hunk loader */
BPTR hunk_load(const char *path, int *err);
void hunk_unload(BPTR seg);
int hunk_where(uint32_t addr, BPTR seg, int *hunk, uint32_t *off);

/* exec */
void exec_init(const char *taskname);
void exec_pending_break(void);
void exec_newlist(void *l);
void exec_addtail(void *l, void *n);

/* RawDoFmt engine */
typedef void (*putch_fn)(int c, void *ctx);
uint32_t fmt_do(const char *fmt, uint32_t data, putch_fn put, void *ctx,
		int bcpl);

/* call a handler-visible Amiga function pointer with registers */
uint32_t amiga_call(struct frame *f, uint32_t pc, struct regs *r);

#define D(n) (f->d[n])
#define A(n) (f->a[n])
#define RET(v) (f->d[0] = (uint32_t)(v))

void af_die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void af_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
