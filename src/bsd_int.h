/* amigafmt: bsdsocket.library internals shared by bsdsocket.c and bsd_*.c */
#ifndef BSD_INT_H
#define BSD_INT_H
#include <stdint.h>
#include "af.h"

/* a socket: a Linux descriptor (always O_NONBLOCK underneath) shared by
   every descriptor-table slot (and every ReleaseCopyOfSocket) made of it */
struct sock {
	int fd;
	int refs;
	int nb;                         /* the program asked for FIONBIO */
	int async;                      /* FIOASYNC (remembered only) */
	int domain, type, proto;
	int64_t rcvtimeo, sndtimeo;     /* microseconds, 0 for none */
};

/* a slot that Dup2Socket(-1, n) reserved: in use, but no socket */
#define SOCK_RESERVED ((struct sock *)1)

/* what hangs off one opener's library base */
struct sb {
	struct Library *base;
	struct sock **dtab;
	int dtsize;
	/* errno as the program sees it (AmiTCP numbers) and where it wants
	   a copy of it */
	int err, herr;
	uint32_t errptr; int errsize;
	uint32_t herrptr;
	uint32_t breakmask, iomask, urgmask, eventmask;
	uint32_t fdcallback;
	/* syslog */
	uint32_t logtag; int logstat, logfac, logmask;
	/* results of the netdb calls, in Amiga memory, until the next one */
	void *dbres;
	char *ntoa;                     /* Inet_NtoA's buffer (amem) */
	struct Task *owner;
};

#ifndef SIGBREAKF_CTRL_C
#define SIGBREAKF_CTRL_C 0x1000
#endif
#define SIGF_SINGLE 0x10                /* our wake-up signal while blocked */

struct sb *sb_of(struct frame *f);
/* errno: a Linux errno becomes the AmiTCP one; the result is -1 */
int sb_errno(struct sb *b, int linux_errno);
void sb_set_errno(struct sb *b, int bsd_errno);
void sb_set_herrno(struct sb *b, int h);
int errno_to_bsd(int e);
const char *bsd_strerror(int bsd);

/* descriptor table */
struct sock *sb_get(struct sb *b, int s);       /* NULL: EBADF/ENOTSOCK set */
int sb_alloc(struct sb *b, struct sock *k, int want);  /* -1: EMFILE set */
int sb_free(struct sb *b, int s);
struct sock *sock_new(int fd, int domain, int type, int proto);
void sock_unref(struct sock *k);

/* wait without stopping the other tasks until the descriptor may be ready
   (0), the deadline passes (0 too: the caller retries and checks), or a
   break signal arrives (BSD_EINTR).  deadline: exec_now_us() time, -1 for
   none. */
int sb_wait_fd(struct sb *b, int fd, short events, int64_t deadline);
/* deadline for a timeout in microseconds, 0 meaning none */
int64_t sb_deadline(int64_t us);

/* sockaddr: Amiga (sa_len, sa_family bytes) <-> Linux (16-bit family) */
struct sockaddr;
int sa_from_amiga(uint32_t a, uint32_t len, struct sockaddr *l, int *llen);
void sa_to_amiga(const struct sockaddr *l, int llen, uint32_t a, uint32_t lenp);

/* the pieces */
extern const struct libimpl bsd_impl_sock[], bsd_impl_io[], bsd_impl_sel[],
	bsd_impl_dup[], bsd_impl_db[];

/* errno values as 4.4BSD (and so AmiTCP) numbers them */
#define BSD_EPERM 1
#define BSD_EINTR 4
#define BSD_EBADF 9
#define BSD_ENOMEM 12
#define BSD_EFAULT 14
#define BSD_EINVAL 22
#define BSD_EMFILE 24
#define BSD_ENOTTY 25
#define BSD_EWOULDBLOCK 35
#define BSD_EINPROGRESS 36
#define BSD_EALREADY 37
#define BSD_ENOTSOCK 38
#define BSD_ENOPROTOOPT 42
#define BSD_EOPNOTSUPP 45
#define BSD_EAFNOSUPPORT 47
#define BSD_ETIMEDOUT 60

#endif
