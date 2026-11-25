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

const struct libimpl bsd_impl_db[] = {
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
