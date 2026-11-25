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

uint32_t call_amiga(struct regs *r, uint32_t pc);
extern uint32_t native_sp;

/* globals */
extern struct ExecBase *SysBase;
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

void af_die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void af_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
