/*
 * bsdsocket.library: copying descriptors and handing sockets to another
 * task (Dup2Socket, ReleaseSocket, ReleaseCopyOfSocket, ObtainSocket).
 *
 * A released socket waits in one list for the whole process (every task
 * here is in it) under its id until some opener's ObtainSocket takes it
 * into its own descriptor table.  UNIQUE_ID (-1) asks for an id nobody
 * else has; those are above 65535, the range a program may pick from
 * itself.
 */
#include <stdlib.h>
#include "bsd_int.h"

struct released {
	int32_t id;
	struct sock *k;
	struct released *next;
};

static struct released *rel;
static int32_t next_unique = 0x10000;

static void b_Dup2Socket(struct frame *f)
{
	struct sb *b = sb_of(f);
	int32_t old = D(0), want = D(1);
	struct sock *k = SOCK_RESERVED;

	if (old != -1) {
		if (!(k = sb_get(b, old))) {
			RET(-1);
			return;
		}
		if (want == old) {
			RET(want);
			return;
		}
	}
	if (want >= b->dtsize || want < -1) {
		sb_set_errno(b, BSD_EBADF);
		RET(-1);
		return;
	}
	if (want >= 0 && b->dtab[want])
		sb_free(b, want);
	want = sb_alloc(b, k, want);
	if (want >= 0 && k != SOCK_RESERVED)
		k->refs++;
	RET(want);
}

static void release(struct frame *f, int copy)
{
	struct sb *b = sb_of(f);
	int32_t s = D(0), id = D(1);
	struct sock *k = sb_get(b, s);
	struct released *r;

	if (!k) {
		RET(-1);
		return;
	}
	if (id == -1)
		id = next_unique++;
	r = malloc(sizeof(*r));
	r->id = id;
	r->k = k;
	r->next = rel;
	rel = r;
	if (copy)
		k->refs++;
	else
		b->dtab[s] = NULL;      /* the reference goes with it */
	RET(id);
}

static void b_ReleaseSocket(struct frame *f) { release(f, 0); }
static void b_ReleaseCopyOfSocket(struct frame *f) { release(f, 1); }

static void b_ObtainSocket(struct frame *f)
{
	struct sb *b = sb_of(f);
	int32_t id = D(0);
	struct released **pp, *r;
	int s;

	for (pp = &rel; (r = *pp); pp = &r->next)
		if (r->id == id)
			break;
	if (!r) {
		sb_set_errno(b, BSD_EWOULDBLOCK);
		RET(-1);
		return;
	}
	s = sb_alloc(b, r->k, -1);
	if (s >= 0) {
		*pp = r->next;
		free(r);
	}
	RET(s);
}

const struct libimpl bsd_impl_dup[] = {
	{ "Dup2Socket", b_Dup2Socket }, { "ReleaseSocket", b_ReleaseSocket },
	{ "ReleaseCopyOfSocket", b_ReleaseCopyOfSocket },
	{ "ObtainSocket", b_ObtainSocket },
	{ NULL, NULL }
};
