/*
 * bsdsocket.library (the AmiTCP API) on Linux sockets.
 *
 * Every OpenLibrary() gets a base of its own (lib_clone), as AmiTCP and
 * Roadshow hand out: a descriptor table, errno and the signal masks belong
 * to the opener.  A socket number is an index into that base's table; the
 * slot holds a struct sock with the Linux descriptor, which is always
 * non-blocking underneath.  A call that should block waits through exec's
 * fd waiters (sb_wait_fd) so the program's other tasks keep running and a
 * break signal (^C) interrupts it with EINTR.
 *
 * This file: the bases, errno, the descriptor table, SocketBaseTagList and
 * the small calls.  bsd_sock.c has socket..getpeername, bsd_io.c the data
 * calls and options, bsd_sel.c WaitSelect, bsd_dup.c passing sockets
 * between tasks, bsd_db.c the netdb and inet_* calls and syslog.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bsd_int.h"

extern uint32_t task_wait(uint32_t mask);
extern struct Task *task_current(void);

#define DTSIZE_DEFAULT 64
#define DTSIZE_MAX 1024

/* ---- errno ---- */

static const int16_t errmap[][2] = {    /* Linux, 4.4BSD */
	{ EAGAIN, 35 }, { EDEADLK, 11 }, { EINPROGRESS, 36 }, { EALREADY, 37 },
	{ ENOTSOCK, 38 }, { EDESTADDRREQ, 39 }, { EMSGSIZE, 40 },
	{ EPROTOTYPE, 41 }, { ENOPROTOOPT, 42 }, { EPROTONOSUPPORT, 43 },
	{ ESOCKTNOSUPPORT, 44 }, { EOPNOTSUPP, 45 }, { EPFNOSUPPORT, 46 },
	{ EAFNOSUPPORT, 47 }, { EADDRINUSE, 48 }, { EADDRNOTAVAIL, 49 },
	{ ENETDOWN, 50 }, { ENETUNREACH, 51 }, { ENETRESET, 52 },
	{ ECONNABORTED, 53 }, { ECONNRESET, 54 }, { ENOBUFS, 55 },
	{ EISCONN, 56 }, { ENOTCONN, 57 }, { ESHUTDOWN, 58 },
	{ ETOOMANYREFS, 59 }, { ETIMEDOUT, 60 }, { ECONNREFUSED, 61 },
	{ ELOOP, 62 }, { ENAMETOOLONG, 63 }, { EHOSTDOWN, 64 },
	{ EHOSTUNREACH, 65 }, { ENOTEMPTY, 66 }, { EUSERS, 68 },
	{ EDQUOT, 69 }, { ESTALE, 70 }, { EREMOTE, 71 }, { ENOLCK, 77 },
	{ ENOSYS, 78 },
};

/* 1..34 (EPERM..ERANGE) are numbered alike, but for EAGAIN */
int errno_to_bsd(int e)
{
	for (unsigned i = 0; i < sizeof(errmap) / sizeof(errmap[0]); i++)
		if (errmap[i][0] == e)
			return errmap[i][1];
	if (e > 0 && e <= 34)
		return e;
	return BSD_EINVAL;
}

static int bsd_to_linux(int b)
{
	for (unsigned i = 0; i < sizeof(errmap) / sizeof(errmap[0]); i++)
		if (errmap[i][1] == b)
			return errmap[i][0];
	return b;
}

/* the text for a BSD errno, in memory the program may keep a pointer to */
const char *bsd_strerror(int bsd)
{
	static char *cache[80];
	const char *s;

	if (bsd < 0 || bsd >= 80)
		return "Unknown error";
	if (!cache[bsd]) {
		s = bsd ? strerror(bsd_to_linux(bsd)) : "Undefined error: 0";
		cache[bsd] = amem_alloc(strlen(s) + 1);
		strcpy(cache[bsd], s);
	}
	return cache[bsd];
}

void sb_set_errno(struct sb *b, int e)
{
	b->err = e;
	if (!b->errptr)
		return;
	switch (b->errsize) {
	case 1: *(uint8_t *)PTR(b->errptr) = e; break;
	case 2: *(uint16_t *)PTR(b->errptr) = e; break;
	default: *(uint32_t *)PTR(b->errptr) = e; break;
	}
}

int sb_errno(struct sb *b, int linux_errno)
{
	sb_set_errno(b, errno_to_bsd(linux_errno));
	return -1;
}

void sb_set_herrno(struct sb *b, int h)
{
	b->herr = h;
	if (b->herrptr)
		*(uint32_t *)PTR(b->herrptr) = h;
}

/* ---- bases ---- */

#define SB_SLOT(base) (*(struct sb **)((char *)(base) + sizeof(struct Library)))

static struct sb *sb_new(struct Library *base)
{
	struct sb *b = amem_alloc(sizeof(*b));

	b->base = base;
	b->dtsize = DTSIZE_DEFAULT;
	b->dtab = calloc(b->dtsize, sizeof(*b->dtab));
	b->breakmask = SIGBREAKF_CTRL_C;
	b->logmask = 0xff;
	b->logfac = 1 << 3;             /* LOG_USER */
	b->owner = task_current();
	b->ntoa = amem_alloc(20);
	SB_SLOT(base) = b;
	return b;
}

struct sb *sb_of(struct frame *f)
{
	struct Library *base = PTR(A(6));
	struct sb *b = SB_SLOT(base);

	/* the shared base, if a program got hold of it some other way */
	return b ? b : sb_new(base);
}

static struct Library *bsd_open(struct amilib *l)
{
	struct Library *base = lib_clone(l);

	sb_new(base);
	return base;
}

static int bsd_close(struct Library *base)
{
	struct sb *b;

	if (!base || !lib_bsdsocket.base || base == lib_bsdsocket.base)
		return 0;
	/* a clone's jump table is a copy of the shared one: same stubs */
	if (memcmp((char *)base - 6, (char *)lib_bsdsocket.base - 6, 6))
		return 0;
	b = SB_SLOT(base);
	if (!b)
		return 0;
	for (int i = 0; i < b->dtsize; i++)
		if (b->dtab[i])
			sb_free(b, i);
	free(b->dtab);
	amem_free(b->dbres);
	amem_free(b->ntoa);
	amem_free(b);
	SB_SLOT(base) = NULL;
	base->lib_OpenCnt = 0;
	return 1;
}

/* ---- the descriptor table ---- */

struct sock *sock_new(int fd, int domain, int type, int proto)
{
	struct sock *k = calloc(1, sizeof(*k));

	k->fd = fd;
	k->refs = 1;
	k->domain = domain;
	k->type = type;
	k->proto = proto;
	return k;
}

void sock_unref(struct sock *k)
{
	if (!k || k == SOCK_RESERVED || --k->refs > 0)
		return;
	close(k->fd);
	free(k);
}

struct sock *sb_get(struct sb *b, int s)
{
	if (s < 0 || s >= b->dtsize || !b->dtab[s]) {
		sb_set_errno(b, BSD_EBADF);
		return NULL;
	}
	if (b->dtab[s] == SOCK_RESERVED) {
		sb_set_errno(b, BSD_ENOTSOCK);
		return NULL;
	}
	return b->dtab[s];
}

/* put k in slot want, or the lowest free one when want is -1 */
int sb_alloc(struct sb *b, struct sock *k, int want)
{
	int s = want;

	if (s < 0)
		for (s = 0; s < b->dtsize && b->dtab[s]; s++)
			;
	if (s < 0 || s >= b->dtsize) {
		sb_set_errno(b, BSD_EMFILE);
		return -1;
	}
	b->dtab[s] = k;
	return s;
}

int sb_free(struct sb *b, int s)
{
	if (s < 0 || s >= b->dtsize || !b->dtab[s]) {
		sb_set_errno(b, BSD_EBADF);
		return -1;
	}
	sock_unref(b->dtab[s]);
	b->dtab[s] = NULL;
	return 0;
}

static int set_dtsize(struct sb *b, int n)
{
	struct sock **t;

	if (n < 1 || n > DTSIZE_MAX)
		return -1;
	for (int i = n; i < b->dtsize; i++)
		if (b->dtab[i])
			return -1;              /* would lose open sockets */
	t = calloc(n, sizeof(*t));
	memcpy(t, b->dtab, (n < b->dtsize ? n : b->dtsize) * sizeof(*t));
	free(b->dtab);
	b->dtab = t;
	b->dtsize = n;
	return 0;
}

/* ---- waiting ---- */

int64_t sb_deadline(int64_t us)
{
	return us > 0 ? exec_now_us() + us : -1;
}

int sb_wait_fd(struct sb *b, int fd, short events, int64_t deadline)
{
	struct pollfd p = { fd, events, 0 };
	struct Task *t = task_current();
	struct exec_fdwait w = {
		.pfd = &p, .n = 1, .deadline = deadline,
		.task = t, .sig = SIGF_SINGLE,
	};
	uint32_t got;

	if (poll(&p, 1, 0) > 0)
		return 0;
	if (t->tc_SigRecvd & b->breakmask)
		return BSD_EINTR;
	t->tc_SigRecvd &= ~SIGF_SINGLE;
	exec_fdwait_add(&w);
	got = task_wait(SIGF_SINGLE | b->breakmask);
	exec_fdwait_remove(&w);
	if (got & b->breakmask & ~SIGF_SINGLE) {
		/* left set, so the program's own CheckSignal() sees the ^C */
		t->tc_SigRecvd |= got & b->breakmask & ~SIGF_SINGLE;
		return BSD_EINTR;
	}
	return 0;
}

/* ---- the small calls ---- */

static void b_Errno(struct frame *f)
{
	RET(sb_of(f)->err);
}

static void b_SetErrnoPtr(struct frame *f)
{
	struct sb *b = sb_of(f);

	if (D(0) != 1 && D(0) != 2 && D(0) != 4) {
		sb_set_errno(b, BSD_EINVAL);
		RET(-1);
		return;
	}
	b->errptr = A(0);
	b->errsize = D(0);
	RET(0);
}

static void b_SetSocketSignals(struct frame *f)
{
	struct sb *b = sb_of(f);

	b->breakmask = D(0);
	b->iomask = D(1);
	b->urgmask = D(2);
}

static void b_getdtablesize(struct frame *f)
{
	RET(sb_of(f)->dtsize);
}

/*
 * SocketBaseTagList: each tag is TAG_USER | code << 1 | set, with 0x8000
 * when the data is a pointer to the value rather than the value.  Returns
 * 0, or the number of the first tag it could not do.
 */
enum {
	SBTC_BREAKMASK = 1, SBTC_SIGIOMASK = 2, SBTC_SIGURGMASK = 3,
	SBTC_SIGEVENTMASK = 4, SBTC_ERRNO = 6, SBTC_HERRNO = 7,
	SBTC_DTABLESIZE = 8, SBTC_FDCALLBACK = 9, SBTC_LOGSTAT = 10,
	SBTC_LOGTAG = 11, SBTC_LOGFACILITY = 12, SBTC_LOGMASK = 13,
	SBTC_ERRNOSTRPTR = 14, SBTC_HERRNOSTRPTR = 15,
	SBTC_IOERRNOSTRPTR = 16, SBTC_S2ERRNOSTRPTR = 17,
	SBTC_S2WERRNOSTRPTR = 18, SBTC_ERRNOBYTEPTR = 21,
	SBTC_ERRNOWORDPTR = 22, SBTC_ERRNOLONGPTR = 24,
	SBTC_HERRNOLONGPTR = 25, SBTC_RELEASESTRPTR = 29,
};

static const char *const herrtext[] = {
	"Resolver Error 0 (no error)", "Unknown host", "Host name lookup failure",
	"Unknown server error", "No address associated with name",
};

/* one tag: 0 when done; *v is the value in or out */
static int sbtag(struct sb *b, int code, int set, uint32_t *v)
{
	uint32_t *slot = NULL;

	switch (code) {
	case SBTC_BREAKMASK: slot = &b->breakmask; break;
	case SBTC_SIGIOMASK: slot = &b->iomask; break;
	case SBTC_SIGURGMASK: slot = &b->urgmask; break;
	case SBTC_SIGEVENTMASK: slot = &b->eventmask; break;
	case SBTC_FDCALLBACK: slot = &b->fdcallback; break;
	case SBTC_LOGTAG: slot = &b->logtag; break;
	case SBTC_ERRNO:
		if (set)
			sb_set_errno(b, *v);
		else
			*v = b->err;
		return 0;
	case SBTC_HERRNO:
		if (set)
			sb_set_herrno(b, *v);
		else
			*v = b->herr;
		return 0;
	case SBTC_DTABLESIZE:
		if (set)
			return set_dtsize(b, *v);
		*v = b->dtsize;
		return 0;
	case SBTC_LOGSTAT:
		if (set) b->logstat = *v; else *v = b->logstat;
		return 0;
	case SBTC_LOGFACILITY:
		if (set) b->logfac = *v; else *v = b->logfac;
		return 0;
	case SBTC_LOGMASK:
		if (set) b->logmask = *v; else *v = b->logmask;
		return 0;
	case SBTC_ERRNOSTRPTR:
		if (set)
			return -1;
		*v = (uint32_t)(uintptr_t)bsd_strerror(*v);
		return 0;
	case SBTC_HERRNOSTRPTR:
		if (set)
			return -1;
		*v = (uint32_t)(uintptr_t)(*v < 5 ? herrtext[*v] : "Unknown resolver error");
		return 0;
	case SBTC_ERRNOBYTEPTR: case SBTC_ERRNOWORDPTR: case SBTC_ERRNOLONGPTR:
		if (!set) {
			*v = b->errptr;
			return 0;
		}
		b->errptr = *v;
		b->errsize = code == SBTC_ERRNOBYTEPTR ? 1 :
			     code == SBTC_ERRNOWORDPTR ? 2 : 4;
		return 0;
	case SBTC_HERRNOLONGPTR:
		if (set) b->herrptr = *v; else *v = b->herrptr;
		return 0;
	case SBTC_RELEASESTRPTR:
		if (set)
			return -1;
		{
			static char *rel;
			if (!rel) {
				const char *s = "amigafmt bsdsocket.library 4.1 (Linux sockets)";
				rel = amem_alloc(strlen(s) + 1);
				strcpy(rel, s);
			}
			*v = (uint32_t)(uintptr_t)rel;
		}
		return 0;
	}
	if (!slot)
		return -1;
	if (set)
		*slot = *v;
	else
		*v = *slot;
	return 0;
}

static void b_SocketBaseTagList(struct frame *f)
{
	struct sb *b = sb_of(f);
	uint32_t *ti = PTR(A(0));
	int n = 0;

	while (ti) {
		uint32_t tag = ti[0], v;
		n++;
		if (tag == 0)                   /* TAG_DONE */
			break;
		if (tag == 1) {                 /* TAG_IGNORE */
			ti += 2;
			continue;
		}
		if (tag == 2) {                 /* TAG_MORE */
			ti = PTR(ti[1]);
			continue;
		}
		if (tag == 3) {                 /* TAG_SKIP */
			ti += 2 * (ti[1] + 1);
			continue;
		}
		if (!(tag & 0x80000000u)) {
			RET(n);
			return;
		}
		{
			int ref = tag & 0x8000, set = tag & 1;
			int code = (tag >> 1) & 0x3fff;
			uint32_t *where = ref ? PTR(ti[1]) : &ti[1];
			v = *where;
			if (sbtag(b, code, set, &v)) {
				RET(n);
				return;
			}
			if (!set)
				*where = v;
		}
		ti += 2;
	}
	RET(0);
}

static const struct libimpl bsd_impl_base[] = {
	{ "Errno", b_Errno }, { "SetErrnoPtr", b_SetErrnoPtr },
	{ "SetSocketSignals", b_SetSocketSignals },
	{ "getdtablesize", b_getdtablesize },
	{ "SocketBaseTagList", b_SocketBaseTagList },
	{ NULL, NULL }
};

static const struct libimpl *const bsd_impls[] = {
	bsd_impl_sock, bsd_impl_io,
	NULL
};

struct amilib lib_bsdsocket = {
	.name = "bsdsocket.library", .version = 4, .revision = 1,
	.fd = fd_bsdsocket, .possize = 8,
	.impl = bsd_impl_base, .impls = bsd_impls,
	.open = bsd_open, .close = bsd_close,
};
