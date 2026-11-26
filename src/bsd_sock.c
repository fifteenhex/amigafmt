/*
 * bsdsocket.library: making, connecting and closing sockets (socket, bind,
 * listen, accept, connect, shutdown, CloseSocket, getsockname,
 * getpeername).
 *
 * An Amiga sockaddr starts with two bytes, sa_len and sa_family, where
 * Linux has a 16-bit family; the rest (port, address, path) is laid out
 * alike, both being big-endian here.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "bsd_int.h"

int sa_from_amiga(uint32_t a, uint32_t len, struct sockaddr *l, int *llen)
{
	const uint8_t *p = PTR(a);

	if (!a || len < 2 || len > sizeof(struct sockaddr_storage))
		return -1;
	memset(l, 0, sizeof(struct sockaddr_storage));
	l->sa_family = p[1];
	memcpy((char *)l + 2, p + 2, len - 2);
	*llen = len;
	return 0;
}

void sa_to_amiga(const struct sockaddr *l, int llen, uint32_t a, uint32_t lenp)
{
	uint8_t *p = PTR(a);
	int32_t *lp = PTR(lenp);
	int room;

	if (!a || !lenp)
		return;
	room = *lp;
	if (room > llen)
		room = llen;
	if (room >= 1)
		p[0] = llen > 255 ? 255 : llen;
	if (room >= 2)
		p[1] = l->sa_family;
	if (room > 2)
		memcpy(p + 2, (const char *)l + 2, room - 2);
	*lp = llen;
}

static void b_socket(struct frame *f)
{
	struct sb *b = sb_of(f);
	int domain = D(0), type = D(1), proto = D(2), fd, s;

	fd = socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
	if (fd < 0) {
		RET(sb_errno(b, errno));
		return;
	}
	s = sb_alloc(b, sock_new(fd, domain, type, proto), -1);
	if (s < 0) {
		close(fd);
		RET(-1);
		return;
	}
	RET(s);
}

static void b_bind(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	struct sockaddr_storage ss;
	int len;

	if (!k) {
		RET(-1);
		return;
	}
	if (sa_from_amiga(A(0), D(1), (struct sockaddr *)&ss, &len)) {
		sb_set_errno(b, BSD_EINVAL);
		RET(-1);
		return;
	}
	RET(bind(k->fd, (struct sockaddr *)&ss, len) ? sb_errno(b, errno) : 0);
}

static void b_listen(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));

	if (!k) {
		RET(-1);
		return;
	}
	RET(listen(k->fd, (int32_t)D(1)) ? sb_errno(b, errno) : 0);
}

static void b_accept(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	struct sockaddr_storage ss;
	socklen_t len;
	int fd, s, e;

	if (!k) {
		RET(-1);
		return;
	}
	for (;;) {
		len = sizeof(ss);
		fd = accept4(k->fd, (struct sockaddr *)&ss, &len,
			     SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd >= 0)
			break;
		if ((errno != EAGAIN && errno != EINTR) || k->nb) {
			RET(sb_errno(b, errno));
			return;
		}
		if ((e = sb_wait_fd(b, k->fd, POLLIN, -1))) {
			sb_set_errno(b, e);
			RET(-1);
			return;
		}
	}
	s = sb_alloc(b, sock_new(fd, k->domain, k->type, k->proto), -1);
	if (s < 0) {
		close(fd);
		RET(-1);
		return;
	}
	sa_to_amiga((struct sockaddr *)&ss, len, A(0), A(1));
	RET(s);
}

static void b_connect(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	struct sockaddr_storage ss;
	socklen_t sl;
	int len, err, e;

	if (!k) {
		RET(-1);
		return;
	}
	if (sa_from_amiga(A(0), D(1), (struct sockaddr *)&ss, &len)) {
		sb_set_errno(b, BSD_EINVAL);
		RET(-1);
		return;
	}
	if (!connect(k->fd, (struct sockaddr *)&ss, len)) {
		RET(0);
		return;
	}
	if (errno != EINPROGRESS || k->nb) {
		RET(sb_errno(b, errno));
		return;
	}
	/* a blocking connect: wait for the outcome */
	if ((e = sb_wait_fd(b, k->fd, POLLOUT, -1))) {
		sb_set_errno(b, e);
		RET(-1);
		return;
	}
	sl = sizeof(err);
	if (getsockopt(k->fd, SOL_SOCKET, SO_ERROR, &err, &sl))
		err = errno;
	RET(err ? sb_errno(b, err) : 0);
}

static void b_shutdown(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));

	if (!k) {
		RET(-1);
		return;
	}
	RET(shutdown(k->fd, D(1)) ? sb_errno(b, errno) : 0);
}

static void b_CloseSocket(struct frame *f)
{
	RET(sb_free(sb_of(f), D(0)));
}

static void name_call(struct frame *f, int peer)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	int r;

	if (!k) {
		RET(-1);
		return;
	}
	r = peer ? getpeername(k->fd, (struct sockaddr *)&ss, &len)
		 : getsockname(k->fd, (struct sockaddr *)&ss, &len);
	if (r) {
		RET(sb_errno(b, errno));
		return;
	}
	sa_to_amiga((struct sockaddr *)&ss, len, A(0), A(1));
	RET(0);
}

static void b_getsockname(struct frame *f) { name_call(f, 0); }
static void b_getpeername(struct frame *f) { name_call(f, 1); }

const struct libimpl bsd_impl_sock[] = {
	{ "socket", b_socket }, { "bind", b_bind }, { "listen", b_listen },
	{ "accept", b_accept }, { "connect", b_connect },
	{ "shutdown", b_shutdown }, { "CloseSocket", b_CloseSocket },
	{ "getsockname", b_getsockname }, { "getpeername", b_getpeername },
	{ NULL, NULL }
};
