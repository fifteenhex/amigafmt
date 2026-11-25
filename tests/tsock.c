/* bsdsocket.library over the loopback: a listener and a client in one
   program, data both ways, WaitSelect with timeouts and signals */
#include "t.h"

/* bsdsocket.library vectors (amitools' bsdsocket_lib.fd) */
#define LVOsocket (-30)
#define LVObind (-36)
#define LVOlisten (-42)
#define LVOaccept (-48)
#define LVOconnect (-54)
#define LVOsend (-66)
#define LVOrecv (-78)
#define LVOshutdown (-84)
#define LVOsetsockopt (-90)
#define LVOgetsockopt (-96)
#define LVOgetsockname (-102)
#define LVOgetpeername (-108)
#define LVOIoctlSocket (-114)
#define LVOCloseSocket (-120)
#define LVOWaitSelect (-126)
#define LVOErrno (-162)
#define LVOSetErrnoPtr (-168)

#define FIONBIO 0x8004667e
#define FIONREAD 0x4004667f
#define SOL_SOCKET 0xffff
#define SO_RCVTIMEO 0x1006
#define SO_TYPE 0x1008

struct sin { u8 len, family; u16 port; u32 addr; u8 zero[8]; };
struct tv { u32 secs, micro; };

static void *sb;
static s32 err;

static void setbit(u32 *set, int s) { set[s / 32] |= 1u << (s % 32); }
static int isset(u32 *set, int s) { return (set[s / 32] >> (s % 32)) & 1; }

/* WaitSelect on one socket for reading (r) or writing (w) */
static s32 wsel(int s, int w, u32 ms, u32 *sigs, u32 *ready)
{
	u32 set[2] = { 0, 0 };
	struct tv tv = { ms / 1000, (ms % 1000) * 1000 };
	s32 n;

	setbit(set, s);
	n = call(sb, LVOWaitSelect, "d0a0a1a2a3d1", s + 1, w ? 0 : (u32)set,
		 w ? (u32)set : 0, 0, ms == ~0u ? 0 : (u32)&tv, (u32)sigs);
	*ready = isset(set, s);
	return n;
}

int main(u32 arglen, char *args)
{
	struct sin a, b, c;
	s32 ls, cs, ss, n, len, v;
	u32 rdy, sig, mask;
	char buf[32];
	struct tv tv;

	sb = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "bsdsocket.library", 4);
	if (!sb) {
		printf("no bsdsocket.library\n");
		return 20;
	}
	call(sb, LVOSetErrnoPtr, "a0d0", &err, 4);

	ls = call(sb, LVOsocket, "d0d1d2", 2, 1, 0);
	memset(&a, 0, sizeof(a));
	a.len = sizeof(a);
	a.family = 2;
	a.addr = 0x7f000001;
	n = call(sb, LVObind, "d0a0d1", ls, &a, sizeof(a));
	v = call(sb, LVOlisten, "d0d1", ls, 5);
	printf("listener %ld, bind %ld, listen %ld\n", ls, n, v);
	len = sizeof(a);
	call(sb, LVOgetsockname, "d0a0a1", ls, &a, &len);
	printf("getsockname: len %ld, sin_len %ld, family %ld, port set %ld\n",
	       len, a.len, a.family, a.port != 0);

	/* a non-blocking connect, finished by WaitSelect for writing */
	cs = call(sb, LVOsocket, "d0d1d2", 2, 1, 0);
	v = 1;
	call(sb, LVOIoctlSocket, "d0d1a0", cs, FIONBIO, &v);
	n = call(sb, LVOconnect, "d0a0d1", cs, &a, sizeof(a));
	printf("connect: %s\n", n == 0 ? "done" :
	       err == 36 ? "in progress" : "failed");
	n = wsel(cs, 1, 2000, 0, &rdy);
	printf("WaitSelect(write) = %ld, ready %ld\n", n, rdy);
	v = 0;
	call(sb, LVOIoctlSocket, "d0d1a0", cs, FIONBIO, &v);

	n = wsel(ls, 0, 2000, 0, &rdy);
	printf("WaitSelect(listener) = %ld, ready %ld\n", n, rdy);
	len = sizeof(b);
	ss = call(sb, LVOaccept, "d0a0a1", ls, &b, &len);
	len = sizeof(c);
	call(sb, LVOgetsockname, "d0a0a1", cs, &c, &len);
	printf("accept %ld: peer is the client %ld\n", ss,
	       b.port == c.port && b.addr == 0x7f000001);
	len = sizeof(c);
	call(sb, LVOgetpeername, "d0a0a1", cs, &c, &len);
	printf("getpeername: the listener %ld\n", c.port == a.port);

	/* nothing to read yet: the timeout */
	n = wsel(ss, 0, 200, 0, &rdy);
	printf("WaitSelect(nothing sent, 200 ms) = %ld, ready %ld\n", n, rdy);

	printf("send = %ld\n", call(sb, LVOsend, "d0a0d1d2", cs, "hello", 5, 0));
	n = wsel(ss, 0, 2000, 0, &rdy);
	call(sb, LVOIoctlSocket, "d0d1a0", ss, FIONREAD, &v);
	printf("WaitSelect(sent) = %ld, ready %ld, FIONREAD %ld\n", n, rdy, v);
	n = call(sb, LVOrecv, "d0a0d1d2", ss, buf, sizeof(buf) - 1, 0);
	buf[n > 0 ? n : 0] = 0;
	printf("recv = %ld \"%s\"\n", n, buf);
	call(sb, LVOsend, "d0a0d1d2", ss, "world!", 6, 0);
	n = call(sb, LVOrecv, "d0a0d1d2", cs, buf, sizeof(buf) - 1, 0);
	buf[n > 0 ? n : 0] = 0;
	printf("reply = %ld \"%s\"\n", n, buf);

	/* options */
	len = 4;
	call(sb, LVOgetsockopt, "d0d1d2a0a1", ss, SOL_SOCKET, SO_TYPE, &v, &len);
	printf("SO_TYPE %ld\n", v);
	tv.secs = 0;
	tv.micro = 100000;
	call(sb, LVOsetsockopt, "d0d1d2a0d3", ss, SOL_SOCKET, SO_RCVTIMEO, &tv, 8);
	n = call(sb, LVOrecv, "d0a0d1d2", ss, buf, sizeof(buf), 0);
	printf("recv with SO_RCVTIMEO = %ld, errno %ld\n", n, err);

	/* errors */
	n = call(sb, LVOsend, "d0a0d1d2", 40, "x", 1, 0);
	printf("send(40) = %ld, errno %ld, Errno() %ld\n", n, err,
	       call(sb, LVOErrno, ""));

	/* a signal wakes WaitSelect and comes back in the mask */
	sig = call(SysBase, LVOAllocSignal, "d0", -1);
	call(SysBase, LVOSignal, "a1d0", call(SysBase, LVOFindTask, "a1", 0),
	     1u << sig);
	mask = 1u << sig;
	n = wsel(ss, 0, ~0u, &mask, &rdy);
	printf("WaitSelect(signalled) = %ld, ready %ld, signal back %ld\n", n, rdy,
	       mask == 1u << sig);
	mask = 1u << sig;
	n = wsel(ss, 0, 100, &mask, &rdy);
	printf("WaitSelect(no signal, 100 ms) = %ld, mask %ld\n", n, mask);

	/* end of stream */
	call(sb, LVOshutdown, "d0d1", cs, 1);
	n = wsel(ss, 0, 2000, 0, &rdy);
	v = call(sb, LVOrecv, "d0a0d1d2", ss, buf, sizeof(buf), 0);
	printf("after shutdown: WaitSelect %ld ready %ld, recv %ld\n", n, rdy, v);

	n = call(sb, LVOCloseSocket, "d0", ss);
	n += call(sb, LVOCloseSocket, "d0", cs);
	n += call(sb, LVOCloseSocket, "d0", ls);
	v = call(sb, LVOCloseSocket, "d0", ls);
	printf("close %ld, again %ld errno %ld\n", n, v, err);
	call(SysBase, LVOCloseLibrary, "a1", sb);
	return 0;
}
