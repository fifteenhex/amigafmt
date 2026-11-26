/*
 * bsdsocket.library: moving data and socket options (send, recv, sendto,
 * recvfrom, sendmsg, recvmsg, setsockopt, getsockopt, IoctlSocket).
 *
 * Everything goes through sendmsg()/recvmsg() on the non-blocking Linux
 * descriptor; a socket the program has not made FIONBIO waits in
 * sb_wait_fd, bounded by SO_SNDTIMEO/SO_RCVTIMEO.  The message flags, the
 * option levels and names and the ioctl numbers are 4.4BSD's and are
 * translated.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "bsd_int.h"

/* ---- message flags ---- */

static const uint32_t msgflags[][2] = {         /* BSD, Linux */
	{ 0x01, MSG_OOB }, { 0x02, MSG_PEEK }, { 0x04, MSG_DONTROUTE },
	{ 0x08, MSG_EOR }, { 0x10, MSG_TRUNC }, { 0x20, MSG_CTRUNC },
	{ 0x40, MSG_WAITALL }, { 0x80, MSG_DONTWAIT },
};

static int flags_to_linux(uint32_t bf)
{
	int lf = 0;

	for (unsigned i = 0; i < sizeof(msgflags) / sizeof(msgflags[0]); i++)
		if (bf & msgflags[i][0])
			lf |= msgflags[i][1];
	return lf;
}

static uint32_t flags_to_bsd(int lf)
{
	uint32_t bf = 0;

	for (unsigned i = 0; i < sizeof(msgflags) / sizeof(msgflags[0]); i++)
		if (lf & msgflags[i][1])
			bf |= msgflags[i][0];
	return bf;
}

/* ---- the transfer loop ---- */

/* one sendmsg/recvmsg, waiting as a blocking socket would */
static int32_t io(struct sb *b, struct sock *k, int out, struct msghdr *m,
		  int lf)
{
	int dontwait = k->nb || (lf & MSG_DONTWAIT), e;
	int64_t dl = sb_deadline(out ? k->sndtimeo : k->rcvtimeo);
	ssize_t r;

	lf &= ~MSG_WAITALL;
	for (;;) {
		r = out ? sendmsg(k->fd, m, lf | MSG_NOSIGNAL | MSG_DONTWAIT)
			: recvmsg(k->fd, m, lf | MSG_DONTWAIT);
		if (r >= 0)
			return r;
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN || dontwait)
			return sb_errno(b, errno);
		if (dl >= 0 && exec_now_us() >= dl) {
			sb_set_errno(b, BSD_EWOULDBLOCK);
			return -1;
		}
		if ((e = sb_wait_fd(b, k->fd, out ? POLLOUT : POLLIN, dl))) {
			sb_set_errno(b, e);
			return -1;
		}
	}
}

/*
 * One buffer.  A blocking send on a stream sends all of it, as BSD does
 * (the Linux side is non-blocking and may take part); MSG_WAITALL makes a
 * stream receive fill the buffer.  name/namelen: a destination (sendto)
 * or where the source goes (recvfrom).
 */
static int32_t xfer(struct frame *f, int out, uint32_t buf, uint32_t len,
		    uint32_t bflags, uint32_t name, uint32_t namelen)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	struct sockaddr_storage ss;
	struct iovec iov;
	struct msghdr m;
	int lf = flags_to_linux(bflags), sl = 0, all;
	int32_t done = 0, r;

	if (!k)
		return -1;
	if (out && name) {
		if (sa_from_amiga(name, namelen, (struct sockaddr *)&ss, &sl)) {
			sb_set_errno(b, BSD_EINVAL);
			return -1;
		}
	}
	all = k->type == SOCK_STREAM && !(lf & MSG_PEEK) &&
	      (out ? !k->nb && !(lf & MSG_DONTWAIT) : (lf & MSG_WAITALL));
	do {
		memset(&m, 0, sizeof(m));
		iov.iov_base = (char *)PTR(buf) + done;
		iov.iov_len = len - done;
		m.msg_iov = &iov;
		m.msg_iovlen = 1;
		if (out && name) {
			m.msg_name = &ss;
			m.msg_namelen = sl;
		} else if (!out && name) {
			m.msg_name = &ss;
			m.msg_namelen = sizeof(ss);
		}
		r = io(b, k, out, &m, lf);
		if (r < 0)
			return done ? done : -1;
		if (!out && name && !done)
			sa_to_amiga((struct sockaddr *)&ss, m.msg_namelen, name, namelen);
		done += r;
	} while (all && r > 0 && done < (int32_t)len);
	return done;
}

static void b_send(struct frame *f)
{
	RET(xfer(f, 1, A(0), D(1), D(2), 0, 0));
}

static void b_recv(struct frame *f)
{
	RET(xfer(f, 0, A(0), D(1), D(2), 0, 0));
}

static void b_sendto(struct frame *f)
{
	RET(xfer(f, 1, A(0), D(1), D(2), A(1), D(3)));
}

static void b_recvfrom(struct frame *f)
{
	RET(xfer(f, 0, A(0), D(1), D(2), A(1), A(2)));
}

/* struct msghdr as the Amiga has it: seven longs */
struct amsghdr {
	uint32_t name, namelen, iov, iovlen, control, controllen;
	int32_t flags;
};

static void msg_call(struct frame *f, int out)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	struct amsghdr *am = PTR(A(0));
	struct sockaddr_storage ss;
	struct msghdr m;
	int sl;
	int32_t r;

	if (!k) {
		RET(-1);
		return;
	}
	memset(&m, 0, sizeof(m));
	/* an Amiga iovec is { base, len } like Linux's on m68k */
	m.msg_iov = PTR(am->iov);
	m.msg_iovlen = am->iovlen;
	if (am->name) {
		if (out) {
			if (sa_from_amiga(am->name, am->namelen,
					  (struct sockaddr *)&ss, &sl)) {
				sb_set_errno(b, BSD_EINVAL);
				RET(-1);
				return;
			}
			m.msg_namelen = sl;
		} else
			m.msg_namelen = sizeof(ss);
		m.msg_name = &ss;
	}
	/* control messages (descriptor passing, options) are not carried */
	r = io(b, k, out, &m, flags_to_linux(D(1)));
	if (r >= 0 && !out) {
		if (am->name) {
			int32_t room = am->namelen;
			sa_to_amiga((struct sockaddr *)&ss, m.msg_namelen,
				    am->name, (uint32_t)(uintptr_t)&room);
			am->namelen = room;
		}
		am->controllen = 0;
		am->flags = flags_to_bsd(m.msg_flags);
	}
	RET(r);
}

static void b_sendmsg(struct frame *f) { msg_call(f, 1); }
static void b_recvmsg(struct frame *f) { msg_call(f, 0); }

/* ---- options ---- */

#define BSD_SOL_SOCKET 0xffff
#define BSD_SO_SNDTIMEO 0x1005
#define BSD_SO_RCVTIMEO 0x1006
#define BSD_SO_ERROR 0x1007
#define BSD_SO_TYPE 0x1008

static const int sockopts[][2] = {             /* BSD, Linux */
	{ 0x0001, SO_DEBUG }, { 0x0002, SO_ACCEPTCONN },
	{ 0x0004, SO_REUSEADDR }, { 0x0008, SO_KEEPALIVE },
	{ 0x0010, SO_DONTROUTE }, { 0x0020, SO_BROADCAST },
	{ 0x0080, SO_LINGER }, { 0x0100, SO_OOBINLINE },
	{ 0x0200, SO_REUSEPORT }, { 0x1001, SO_SNDBUF },
	{ 0x1002, SO_RCVBUF }, { 0x1003, SO_SNDLOWAT },
	{ 0x1004, SO_RCVLOWAT }, { BSD_SO_ERROR, SO_ERROR },
};

static const int ipopts[][2] = {
	{ 1, IP_OPTIONS }, { 2, IP_HDRINCL }, { 3, IP_TOS }, { 4, IP_TTL },
	{ 5, IP_RECVOPTS }, { 6, IP_RECVRETOPTS }, { 8, IP_RETOPTS },
	{ 9, IP_MULTICAST_IF }, { 10, IP_MULTICAST_TTL },
	{ 11, IP_MULTICAST_LOOP }, { 12, IP_ADD_MEMBERSHIP },
	{ 13, IP_DROP_MEMBERSHIP },
};

/* level and name to Linux's; 0, or -1 for an option there is no such of */
static int opt_map(int *level, int *name)
{
	const int (*t)[2];
	int n;

	if (*level == BSD_SOL_SOCKET) {
		*level = SOL_SOCKET;
		t = sockopts;
		n = sizeof(sockopts) / sizeof(sockopts[0]);
	} else if (*level == IPPROTO_IP) {
		t = ipopts;
		n = sizeof(ipopts) / sizeof(ipopts[0]);
	} else
		return 0;                       /* TCP, UDP: numbered alike */
	for (int i = 0; i < n; i++)
		if (t[i][0] == *name) {
			*name = t[i][1];
			return 0;
		}
	return -1;
}

static void b_setsockopt(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	int level = D(1), name = D(2);
	uint32_t *tv = PTR(A(0));

	if (!k) {
		RET(-1);
		return;
	}
	if (level == BSD_SOL_SOCKET &&
	    (name == BSD_SO_SNDTIMEO || name == BSD_SO_RCVTIMEO)) {
		/* ours: the Linux side never blocks */
		if (!tv || D(3) < 8) {
			sb_set_errno(b, BSD_EINVAL);
			RET(-1);
			return;
		}
		*(name == BSD_SO_SNDTIMEO ? &k->sndtimeo : &k->rcvtimeo) =
			(int64_t)tv[0] * 1000000 + tv[1];
		RET(0);
		return;
	}
	if (opt_map(&level, &name)) {
		sb_set_errno(b, BSD_ENOPROTOOPT);
		RET(-1);
		return;
	}
	RET(setsockopt(k->fd, level, name, PTR(A(0)), D(3)) ?
	    sb_errno(b, errno) : 0);
}

static void b_getsockopt(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	int level = D(1), name = D(2);
	int32_t *lenp = PTR(A(1));
	socklen_t len;
	uint32_t val[2];

	if (!k) {
		RET(-1);
		return;
	}
	if (!A(0) || !lenp) {
		sb_set_errno(b, BSD_EFAULT);
		RET(-1);
		return;
	}
	if (level == BSD_SOL_SOCKET &&
	    (name == BSD_SO_SNDTIMEO || name == BSD_SO_RCVTIMEO ||
	     name == BSD_SO_TYPE)) {
		int64_t us = name == BSD_SO_SNDTIMEO ? k->sndtimeo : k->rcvtimeo;
		if (name == BSD_SO_TYPE) {
			val[0] = k->type;
			len = 4;
		} else {
			val[0] = us / 1000000;
			val[1] = us % 1000000;
			len = 8;
		}
		if (*lenp < (int32_t)len)
			len = *lenp;
		memcpy(PTR(A(0)), val, len);
		*lenp = len;
		RET(0);
		return;
	}
	if (opt_map(&level, &name)) {
		sb_set_errno(b, BSD_ENOPROTOOPT);
		RET(-1);
		return;
	}
	len = *lenp;
	if (getsockopt(k->fd, level, name, PTR(A(0)), &len)) {
		RET(sb_errno(b, errno));
		return;
	}
	if (level == SOL_SOCKET && name == SO_ERROR && len == 4)
		*(int32_t *)PTR(A(0)) = *(int32_t *)PTR(A(0)) ?
			errno_to_bsd(*(int32_t *)PTR(A(0))) : 0;
	*lenp = len;
	RET(0);
}

/* ---- IoctlSocket ---- */

#define BSD_FIOCLEX     0x20006601
#define BSD_FIONCLEX    0x20006602
#define BSD_FIONBIO     0x8004667e
#define BSD_FIOASYNC    0x8004667d
#define BSD_FIONREAD    0x4004667f
#define BSD_SIOCATMARK  0x40047307
#define BSD_SIOCSPGRP   0x80047308
#define BSD_SIOCGPGRP   0x40047309

static void b_IoctlSocket(struct frame *f)
{
	struct sb *b = sb_of(f);
	struct sock *k = sb_get(b, D(0));
	int32_t *arg = PTR(A(0));
	int v;

	if (!k) {
		RET(-1);
		return;
	}
	switch (D(1)) {
	case BSD_FIOCLEX: case BSD_FIONCLEX:
		break;
	case BSD_FIONBIO:
		k->nb = arg && *arg;
		break;
	case BSD_FIOASYNC:
		k->async = arg && *arg;
		break;
	case BSD_SIOCSPGRP:
		break;
	case BSD_SIOCGPGRP:
		if (arg)
			*arg = 0;
		break;
	case BSD_FIONREAD:
	case BSD_SIOCATMARK:
		if (ioctl(k->fd, D(1) == BSD_FIONREAD ? FIONREAD : SIOCATMARK, &v)) {
			RET(sb_errno(b, errno));
			return;
		}
		if (arg)
			*arg = v;
		break;
	default:
		sb_set_errno(b, BSD_EINVAL);
		RET(-1);
		return;
	}
	RET(0);
}

const struct libimpl bsd_impl_io[] = {
	{ "send", b_send }, { "recv", b_recv },
	{ "sendto", b_sendto }, { "recvfrom", b_recvfrom },
	{ "sendmsg", b_sendmsg }, { "recvmsg", b_recvmsg },
	{ "setsockopt", b_setsockopt }, { "getsockopt", b_getsockopt },
	{ "IoctlSocket", b_IoctlSocket },
	{ NULL, NULL }
};
