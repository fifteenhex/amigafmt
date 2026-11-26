/*
 * bsdsocket.library: WaitSelect, select() that also waits for Amiga
 * signals.
 *
 * An Amiga fd_set is an array of longs, socket n being bit n % 32 of long
 * n / 32.  The descriptors are polled once; if nothing is ready the task
 * sleeps through exec's fd waiters, so other tasks run and anything that
 * sets a signal (^C, wbde's events, timer.device, another task) wakes it.
 * Signals in *sigmask that arrive are returned there and cleared, as
 * Wait() would; a break signal not in it fails the call with EINTR.
 */
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include "bsd_int.h"

extern uint32_t task_wait(uint32_t mask);
extern struct Task *task_current(void);

static int isset(const uint32_t *set, int s)
{
	return set && (set[s / 32] & (1u << (s % 32)));
}

static void b_WaitSelect(struct frame *f)
{
	struct sb *b = sb_of(f);
	int nfds = (int32_t)D(0), np = 0, n = 0;
	uint32_t *rs = PTR(A(0)), *ws = PTR(A(1)), *es = PTR(A(2));
	uint32_t *tv = PTR(A(3)), *sm = PTR(D(1));
	uint32_t user = sm ? *sm : 0, brk, got = 0;
	struct Task *t = task_current();
	struct pollfd *p;
	int *slot;
	int64_t deadline = -1;

	if (nfds < 0) {
		sb_set_errno(b, BSD_EINVAL);
		RET(-1);
		return;
	}
	if (nfds > b->dtsize)
		nfds = b->dtsize;
	brk = b->breakmask & ~user;
	p = calloc(nfds + 1, sizeof(*p));
	slot = calloc(nfds + 1, sizeof(*slot));
	for (int s = 0; s < nfds; s++) {
		short ev = (isset(rs, s) ? POLLIN : 0) |
			   (isset(ws, s) ? POLLOUT : 0) |
			   (isset(es, s) ? POLLPRI : 0);
		struct sock *k;
		if (!ev)
			continue;
		if (!(k = sb_get(b, s))) {
			sb_set_errno(b, BSD_EBADF);
			RET(-1);
			goto out;
		}
		p[np] = (struct pollfd){ k->fd, ev, 0 };
		slot[np++] = s;
	}
	if (tv)
		deadline = exec_now_us() + (int64_t)tv[0] * 1000000 + tv[1];

	for (;;) {
		int r = poll(p, np, 0);
		if (r > 0)
			break;
		got = t->tc_SigRecvd & user;
		if (got)
			break;
		if (t->tc_SigRecvd & brk) {
			sb_set_errno(b, BSD_EINTR);
			RET(-1);
			goto out;
		}
		if (deadline >= 0 && exec_now_us() >= deadline)
			break;
		{
			struct exec_fdwait w = {
				.pfd = p, .n = np, .deadline = deadline,
				.task = t, .sig = SIGF_SINGLE,
			};
			uint32_t g;
			t->tc_SigRecvd &= ~SIGF_SINGLE;
			exec_fdwait_add(&w);
			g = task_wait(SIGF_SINGLE | user | brk);
			exec_fdwait_remove(&w);
			/* put them back: the top of the loop sorts them out */
			t->tc_SigRecvd |= g & ~SIGF_SINGLE;
			for (int i = 0; i < np; i++)
				p[i].revents = 0;
		}
	}

	/* the answer: which descriptors, which signals */
	got = t->tc_SigRecvd & user;
	t->tc_SigRecvd &= ~got;
	if (sm)
		*sm = got;
	for (int i = 0; i < (nfds + 31) / 32; i++) {
		if (rs) rs[i] = 0;
		if (ws) ws[i] = 0;
		if (es) es[i] = 0;
	}
	for (int i = 0; i < np; i++) {
		short re = p[i].revents, ev = p[i].events;
		uint32_t bit = 1u << (slot[i] % 32);
		int w = slot[i] / 32;
		if ((ev & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
			rs[w] |= bit;
			n++;
		}
		if ((ev & POLLOUT) && (re & (POLLOUT | POLLHUP | POLLERR | POLLNVAL))) {
			ws[w] |= bit;
			n++;
		}
		if ((ev & POLLPRI) && (re & POLLPRI)) {
			es[w] |= bit;
			n++;
		}
	}
	RET(n);
out:
	free(p);
	free(slot);
	return;
}

const struct libimpl bsd_impl_sel[] = {
	{ "WaitSelect", b_WaitSelect },
	{ NULL, NULL }
};
