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
uint32_t call_amiga_super(struct regs *r, uint32_t pc);
extern int in_supervisor;
extern int dispatch_all_regs;
extern uint32_t native_sp;

/* a library vector's implementation */
typedef void (*lvo_fn)(struct frame *f);

struct fdent {
	uint16_t bias;
	const char *name, *regs;
};

extern const struct fdent fd_exec[], fd_dos[], fd_utility[], fd_intuition[],
	fd_graphics[], fd_locale[], fd_mathieeedoubbas[], fd_mathieeedoubtrans[],
	fd_mathieeesingbas[], fd_mathieeesingtrans[], fd_mathffp[], fd_mathtrans[],
	fd_icon[], fd_iffparse[];

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
	int hidden;                     /* internal: in no exec list */
	const struct libimpl *impl2;    /* more implementations */
	const struct libimpl *impl3;    /* and more, bound last */
	/* filled in by lib_build() */
	struct Library *base;
	int nslots;
	lvo_fn *table;
	int index;
};

extern struct amilib lib_exec, lib_dos, lib_utility, lib_version,
	lib_intuition, lib_graphics, lib_gadtools, lib_mathieeedoubbas,
	lib_mathieeedoubtrans, lib_mathieeesingbas, lib_mathieeesingtrans,
	lib_mathffp, lib_mathtrans, lib_timer, lib_icon, lib_layers, lib_console, lib_asl, lib_iffparse, lib_boopsi, lib_workbench, lib_datatypes,
	lib_diskfont, lib_locale, lib_keymap, lib_input;
extern struct amilib *all_libs[];

struct Library *lib_build(struct amilib *l);
struct amilib *lib_find(const char *name);
const char *lib_fn_name(struct amilib *l, int slot, const char **regs);
struct frame *amiga_dispatch(struct frame *f);
extern struct frame *resume_frame;

/* globals */
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
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

/* path translation */
void path_init(const char *progpath);
int path_assign(const char *name, const char *target);
char *path_to_linux(const char *apath, const char *cwd);
const char *path_cwd(void);
void path_set_cwd(const char *lpath);
char *path_to_amiga(const char *lpath);
void path_dump(FILE *f);

/* timer.device */
int64_t timer_poll(void);
void timer_sleep(int64_t us);

/* exec */
void exec_init(void);
void exec_pending_break(void);

/* dos */
void dos_init(int argc, char **argv, const char *prog, BPTR seg, uint32_t stack);
void dos_flush_all(void);
void dos_set_ioerr(int32_t e);
int32_t dos_errno(int e);
void dos_exit(int32_t code);
extern char *dos_args;
extern uint32_t dos_arglen;
void dos_preload_args(const char *s, uint32_t len);
int dos_take_args(const char **s, int32_t *len);

/* RawDoFmt engine shared by exec and dos */
typedef void (*putch_fn)(int c, void *ctx);
uint32_t fmt_do(const char *fmt, uint32_t data, putch_fn put, void *ctx,
		int bcpl);

/* ReadArgs pieces */
struct frame;
uint32_t ra_readargs(const char *tmpl, int32_t *array, struct RDArgs *rd);
void ra_freeargs(struct RDArgs *rd);
int32_t ra_readitem(char *buf, int32_t max, struct CSource *cs);
int32_t ra_findarg(const char *tmpl, const char *key);

/* pattern matching */
int32_t pat_parse(const char *src, char *dst, int32_t len, int nocase);
int pat_match(const char *pat, const char *str, int nocase);
int pat_matchraw(const char *pat, const char *str, int nocase);

/* date */
void date_now(struct DateStamp *ds);
void date_from_unix(int64_t t, struct DateStamp *ds);
int64_t date_to_unix(const struct DateStamp *ds);

/* call a handler-visible Amiga function pointer with registers */
uint32_t amiga_call(struct frame *f, uint32_t pc, struct regs *r);
uint32_t amiga_call_super(struct frame *f, uint32_t pc, struct regs *r);

#define D(n) (f->d[n])
#define A(n) (f->a[n])
#define RET(v) (f->d[0] = (uint32_t)(v))

void af_die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void af_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
