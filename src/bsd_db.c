/*
 * bsdsocket.library: the network databases (gethostby*, getserv*,
 * getproto*, getnet*, gethostname, gethostid), the inet_* address calls
 * and syslog.
 *
 * Lookups are netdb.c's.  A result is built as the Amiga structure in one
 * block of Amiga memory per base, valid until that base's next lookup, as
 * the static buffer of a BSD libc is.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "bsd_int.h"
#include "netdb_int.h"

/* ---- building results ---- */

struct bump {
	char *mem;
	uint32_t off, size;
};

static int db_new(struct sb *b, struct bump *bp, uint32_t size)
{
	amem_free(b->dbres);
	b->dbres = amem_alloc(size);
	bp->mem = b->dbres;
	bp->off = 0;
	bp->size = size;
	return bp->mem != NULL;
}

static void *bput(struct bump *bp, uint32_t n)
{
	void *p;

	n = (n + 3) & ~3u;
	if (bp->off + n > bp->size)
		return NULL;
	p = bp->mem + bp->off;
	bp->off += n;
	return p;
}

static uint32_t bstr(struct bump *bp, const char *s)
{
	char *p = bput(bp, strlen(s) + 1);

	if (!p)
		return 0;
	strcpy(p, s);
	return (uint32_t)(uintptr_t)p;
}

/* a NULL-ended array of string pointers */
static uint32_t bstrs(struct bump *bp, const char *s, int stride, int n)
{
	uint32_t *v = bput(bp, (n + 1) * 4);

	if (!v)
		return 0;
	for (int i = 0; i < n; i++)
		v[i] = bstr(bp, s + i * stride);
	v[n] = 0;
	return (uint32_t)(uintptr_t)v;
}

#define DBSIZE (16 * 1024)

/* struct hostent: h_name, h_aliases, h_addrtype, h_length, h_addr_list */
static uint32_t make_hostent(struct sb *b, const struct nd_host *h)
{
	struct bump bp;
	uint32_t *he, *al, *ad;

	if (!db_new(b, &bp, DBSIZE))
		return 0;
	he = bput(&bp, 20);
	he[0] = bstr(&bp, h->name);
	he[1] = bstrs(&bp, h->aliases[0], sizeof(h->aliases[0]),
		      h->nalias);
	he[2] = AF_INET;
	he[3] = 4;
	al = bput(&bp, (h->naddr + 1) * 4);
	ad = bput(&bp, h->naddr * 4 + 4);
	for (int i = 0; i < h->naddr; i++) {
		ad[i] = h->addr[i];
		al[i] = (uint32_t)(uintptr_t)&ad[i];
	}
	al[h->naddr] = 0;
	he[4] = (uint32_t)(uintptr_t)al;
	return (uint32_t)(uintptr_t)he;
}

static void host_result(struct frame *f, struct sb *b, int r,
			const struct nd_host *h)
{
	if (r) {
		sb_set_herrno(b, r);
		RET(0);
		return;
	}
	sb_set_herrno(b, 0);
	RET(make_hostent(b, h));
}

static void b_gethostbyname(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_host h;

	host_result(f, b, nd_gethostbyname(PTR(A(0)), &h), &h);
}

static void b_gethostbyaddr(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_host h;
	uint32_t addr;

	if (!A(0) || D(0) != 4 || D(1) != AF_INET) {
		sb_set_herrno(b, ND_HOST_NOT_FOUND);
		RET(0);
		return;
	}
	memcpy(&addr, PTR(A(0)), 4);
	host_result(f, b, nd_gethostbyaddr(addr, &h), &h);
}

/* servent, protoent and netent: name, aliases, then one or two longs */
static uint32_t make_ent(struct sb *b, const struct nd_ent *e, int kind)
{
	struct bump bp;
	uint32_t *v;

	if (!db_new(b, &bp, DBSIZE))
		return 0;
	v = bput(&bp, 16);
	v[0] = bstr(&bp, e->name);
	v[1] = bstrs(&bp, e->aliases[0], sizeof(e->aliases[0]),
		     e->nalias);
	switch (kind) {
	case 's':               /* s_port (network order), s_proto */
		v[2] = htons(e->num);
		v[3] = bstr(&bp, e->proto);
		break;
	case 'p':               /* p_proto */
		v[2] = e->num;
		break;
	case 'n':               /* n_addrtype, n_net */
		v[2] = AF_INET;
		v[3] = e->num;
		break;
	}
	return (uint32_t)(uintptr_t)v;
}

static void b_getservbyname(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_ent e;

	RET(A(0) && nd_getserv(PTR(A(0)), -1, PTR(A(1)), &e) ?
	    make_ent(b, &e, 's') : 0);
}

static void b_getservbyport(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_ent e;

	RET(nd_getserv(NULL, ntohs(D(0) & 0xffff), PTR(A(0)), &e) ?
	    make_ent(b, &e, 's') : 0);
}

static void b_getprotobyname(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_ent e;

	RET(A(0) && nd_getproto(PTR(A(0)), 0, &e) ? make_ent(b, &e, 'p') : 0);
}

static void b_getprotobynumber(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_ent e;

	RET(nd_getproto(NULL, D(0), &e) ? make_ent(b, &e, 'p') : 0);
}

static void b_getnetbyname(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_ent e;

	RET(A(0) && nd_getnet(PTR(A(0)), 0, &e) ? make_ent(b, &e, 'n') : 0);
}

static void b_getnetbyaddr(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct nd_ent e;

	RET(D(1) == AF_INET && nd_getnet(NULL, D(0), &e) ?
	    make_ent(b, &e, 'n') : 0);
}

static void b_gethostname(struct frame *f)
{
	struct sb *b = sb_of(f);
	char name[256];
	uint32_t n = D(0);

	if (gethostname(name, sizeof(name))) {
		RET(sb_errno(b, errno));
		return;
	}
	name[sizeof(name) - 1] = 0;
	if (n) {
		strncpy(PTR(A(0)), name, n);
		((char *)PTR(A(0)))[n - 1] = 0;
	}
	RET(0);
}

/* the address this machine would send from, found by routing a UDP
   socket somewhere (connect() on UDP sends nothing); else loopback */
static void b_gethostid(struct frame *f)
{
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9) };
	struct sockaddr_in me;
	socklen_t len = sizeof(me);
	uint32_t id = htonl(INADDR_LOOPBACK);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	to.sin_addr.s_addr = htonl(0xc0000201);         /* 192.0.2.1 */
	if (fd >= 0) {
		if (!connect(fd, (struct sockaddr *)&to, sizeof(to)) &&
		    !getsockname(fd, (struct sockaddr *)&me, &len) &&
		    me.sin_addr.s_addr)
			id = me.sin_addr.s_addr;
		close(fd);
	}
	RET(id);
}

/* ---- addresses ---- */

static void b_Inet_NtoA(struct frame *f)
{
	struct sb *b = sb_of(f);
	uint32_t a = D(0);

	snprintf(b->ntoa, 20, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255,
		 (a >> 8) & 255, a & 255);
	RET(b->ntoa);
}

static void b_inet_addr(struct frame *f)
{
	struct in_addr ia;

	RET(A(0) && inet_aton(PTR(A(0)), &ia) ? ia.s_addr : INADDR_NONE);
}

static void b_inet_network(struct frame *f)
{
	int ok;
	uint32_t v = A(0) ? nd_inet_network(PTR(A(0)), &ok) : 0;

	RET(A(0) && ok ? v : INADDR_NONE);
}

/* the classful split, as 4.4BSD's inet_lnaof/inet_netof/inet_makeaddr */
static void b_Inet_LnaOf(struct frame *f)
{
	uint32_t i = D(0);

	RET(!(i & 0x80000000) ? i & 0x00ffffff :
	    (i & 0xc0000000) == 0x80000000 ? i & 0x0000ffff : i & 0x000000ff);
}

static void b_Inet_NetOf(struct frame *f)
{
	uint32_t i = D(0);

	RET(!(i & 0x80000000) ? i >> 24 :
	    (i & 0xc0000000) == 0x80000000 ? i >> 16 : i >> 8);
}

static void b_Inet_MakeAddr(struct frame *f)
{
	uint32_t net = D(0), host = D(1);

	RET(net < 128 ? net << 24 | (host & 0x00ffffff) :
	    net < 65536 ? net << 16 | (host & 0x0000ffff) :
	    net < 16777216 ? net << 8 | (host & 0x000000ff) : net | host);
}

/* ---- syslog ---- */

struct logbuf {
	char s[1024];
	int n;
};

static void logput(int c, void *ctx)
{
	struct logbuf *l = ctx;

	if (c && l->n < (int)sizeof(l->s) - 1)
		l->s[l->n++] = c;
}

/*
 * vsyslog(level, format, args) goes to standard error as "tag: message".
 * The format is RawDoFmt's, as the Amiga's syslog takes it, with %m for
 * the text of errno.
 */
static void b_vsyslog(struct frame *f)
{
	struct sb *b = sb_of(f);
	const char *fmt = PTR(A(0)), *tag;
	char fixed[1024];
	struct logbuf l = { .n = 0 };
	int pri = D(0) & 7, o = 0;

	if (!fmt || !(b->logmask & (1 << pri)))
		return;
	for (; *fmt && o < (int)sizeof(fixed) - 1; fmt++) {
		if (fmt[0] == '%' && fmt[1] == 'm') {
			for (const char *e = bsd_strerror(b->err);
			     *e && o < (int)sizeof(fixed) - 2; e++) {
				if (*e == '%')
					fixed[o++] = '%';
				fixed[o++] = *e;
			}
			fmt++;
		} else if (fmt[0] == '%' && fmt[1] == '%') {
			fixed[o++] = *fmt++;
			fixed[o++] = *fmt;
		} else
			fixed[o++] = *fmt;
	}
	fixed[o] = 0;
	fmt_do(fixed, A(1), logput, &l, 0);
	while (l.n && l.s[l.n - 1] == '\n')
		l.n--;
	l.s[l.n] = 0;
	tag = b->logtag ? PTR(b->logtag) :
	      b->owner && b->owner->tc_Node.ln_Name ?
	      (const char *)PTR(b->owner->tc_Node.ln_Name) : af_progname;
	dos_flush_all();
	if (b->logstat & 1)                     /* LOG_PID */
		fprintf(stderr, "%s[%d]: %s\n", tag, (int)getpid(), l.s);
	else
		fprintf(stderr, "%s: %s\n", tag, l.s);
	fflush(stderr);
}

const struct libimpl bsd_impl_db[] = {
	{ "gethostname", b_gethostname }, { "gethostid", b_gethostid },
	{ "Inet_NtoA", b_Inet_NtoA }, { "inet_addr", b_inet_addr },
	{ "inet_network", b_inet_network }, { "Inet_LnaOf", b_Inet_LnaOf },
	{ "Inet_NetOf", b_Inet_NetOf }, { "Inet_MakeAddr", b_Inet_MakeAddr },
	{ "vsyslog", b_vsyslog },
	{ "gethostbyname", b_gethostbyname },
	{ "gethostbyaddr", b_gethostbyaddr },
	{ "getservbyname", b_getservbyname },
	{ "getservbyport", b_getservbyport },
	{ "getprotobyname", b_getprotobyname },
	{ "getprotobynumber", b_getprotobynumber },
	{ "getnetbyname", b_getnetbyname },
	{ "getnetbyaddr", b_getnetbyaddr },
	{ NULL, NULL }
};
