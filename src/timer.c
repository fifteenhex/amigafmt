/*
 * timer.device, and exec's device I/O (DoIO, SendIO, WaitIO, CheckIO,
 * AbortIO) for it.  The only device there is.
 *
 * TR_ADDREQUEST requests wait in a list sorted by deadline; they complete
 * (ReplyMsg, so the reply port's signal is set) whenever exec looks:
 * Wait() sleeps until the earliest deadline or a ^C, WaitIO() until its
 * own.  TR_GETSYSTIME gives AmigaOS time (seconds since 1978).
 *
 * Offsets: AddTime..GetSysTime from timer_lib.fd (bias 42); BeginIO -30
 * and AbortIO -36 are every device's.
 */
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include "af.h"

#define CMD_INVALID     0
#define TR_ADDREQUEST   9
#define TR_GETSYSTIME   10
#define TR_SETSYSTIME   11
#define UNIT_MICROHZ    0
#define UNIT_VBLANK     1
#define UNIT_ECLOCK     2
#define UNIT_WAITUNTIL  3
#define UNIT_WAITECLOCK 4
#define IOF_QUICK       1
#define IOERR_OPENFAIL  (-1)
#define IOERR_ABORTED   (-2)
#define IOERR_NOCMD     (-3)
#define ECLOCK          709379

/* struct IORequest / timerequest, as offsets */
struct ioreq {
	struct Message io_Message;
	APTR io_Device, io_Unit;
	uint16_t io_Command;
	uint8_t io_Flags;
	int8_t io_Error;
	uint32_t tv_secs, tv_micro;
};

struct pending {
	struct ioreq *io;
	uint64_t due;           /* microseconds, CLOCK_MONOTONIC */
	struct pending *next;
};

static struct pending *queue;
static struct Library *TimerBase;

extern void exec_reply(struct Message *m);

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* seconds since 1978, as GetSysTime() and the WAITUNTIL unit use */
static uint64_t amiga_now_us(void)
{
	struct timeval tv;
	struct DateStamp ds;

	gettimeofday(&tv, NULL);
	date_from_unix(tv.tv_sec, &ds);
	return ((uint64_t)ds.ds_Days * 86400 + ds.ds_Minute * 60 +
		ds.ds_Tick / 50) * 1000000 + tv.tv_usec;
}

static void complete(struct ioreq *io)
{
	io->io_Message.mn_Node.ln_Type = 7;     /* NT_REPLYMSG */
	if (!(io->io_Flags & IOF_QUICK))
		exec_reply(&io->io_Message);
}

static int dequeue(struct ioreq *io)
{
	struct pending **pp, *p;

	for (pp = &queue; (p = *pp); pp = &p->next)
		if (p->io == io) {
			*pp = p->next;
			amem_free(p);
			return 1;
		}
	return 0;
}

/* finish everything that is due; returns microseconds to the next one,
   or -1 when nothing waits */
int64_t timer_poll(void)
{
	uint64_t t = now_us();

	while (queue && queue->due <= t) {
		struct pending *p = queue;
		queue = p->next;
		p->io->io_Error = 0;
		complete(p->io);
		amem_free(p);
	}
	return queue ? (int64_t)(queue->due - t) : -1;
}

void timer_sleep(int64_t us)
{
	struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };

	dos_flush_all();
	nanosleep(&ts, NULL);   /* a signal (^C) cuts it short: fine */
}

static void begin_io(struct ioreq *io)
{
	struct pending *p, **pp;
	uint64_t us, due;

	io->io_Message.mn_Node.ln_Type = 5;     /* NT_MESSAGE: in progress */
	io->io_Error = 0;
	switch (io->io_Command) {
	case TR_GETSYSTIME:
		us = amiga_now_us();
		io->tv_secs = us / 1000000;
		io->tv_micro = us % 1000000;
		complete(io);
		return;
	case TR_SETSYSTIME:
		complete(io);           /* refused silently: not our clock */
		return;
	case TR_ADDREQUEST:
		break;
	default:
		io->io_Error = IOERR_NOCMD;
		complete(io);
		return;
	}
	us = (uint64_t)io->tv_secs * 1000000 + io->tv_micro;
	switch ((uint32_t)io->io_Unit) {
	case UNIT_WAITUNTIL: {
		uint64_t a = amiga_now_us();
		due = now_us() + (us > a ? us - a : 0);
		break;
	}
	case UNIT_ECLOCK:
	case UNIT_WAITECLOCK:   /* tv is an EClockVal: hi, lo ticks */
		us = (((uint64_t)io->tv_secs << 32) | io->tv_micro) * 1000000 /
		     ECLOCK;
		if ((uint32_t)io->io_Unit == UNIT_WAITECLOCK) {
			uint64_t e = now_us();
			due = us > e ? us : e;
		} else
			due = now_us() + us;
		break;
	default:
		due = now_us() + us;
	}
	io->io_Flags &= ~IOF_QUICK;             /* it will be replied */
	p = amem_alloc(sizeof(*p));
	p->io = io;
	p->due = due;
	for (pp = &queue; *pp && (*pp)->due <= due; pp = &(*pp)->next)
		;
	p->next = *pp;
	*pp = p;
}

/* ---- the device's vectors ---- */

static void t_BeginIO(struct frame *f) { begin_io(PTR(A(1))); }

static void t_AbortIO(struct frame *f)
{
	struct ioreq *io = PTR(A(1));

	if (dequeue(io)) {
		io->io_Error = IOERR_ABORTED;
		complete(io);
	}
	RET(0);
}

struct timeval32 {
	uint32_t secs, micro;
};

static void t_AddTime(struct frame *f)
{
	struct timeval32 *d = PTR(A(0)), *s = PTR(A(1));

	d->micro += s->micro;
	d->secs += s->secs + d->micro / 1000000;
	d->micro %= 1000000;
}

static void t_SubTime(struct frame *f)
{
	struct timeval32 *d = PTR(A(0)), *s = PTR(A(1));

	if (d->micro < s->micro) {
		d->micro += 1000000;
		d->secs--;
	}
	d->micro -= s->micro;
	d->secs -= s->secs;
}

static void t_CmpTime(struct frame *f)
{
	struct timeval32 *d = PTR(A(0)), *s = PTR(A(1));

	if (d->secs != s->secs)
		RET(d->secs > s->secs ? -1 : 1);
	else if (d->micro != s->micro)
		RET(d->micro > s->micro ? -1 : 1);
	else
		RET(0);
}

static void t_ReadEClock(struct frame *f)
{
	uint32_t *e = PTR(A(0));
	uint64_t ticks = now_us() * ECLOCK / 1000000;

	e[0] = ticks >> 32;
	e[1] = (uint32_t)ticks;
	RET(ECLOCK);
}

static void t_GetSysTime(struct frame *f)
{
	struct timeval32 *d = PTR(A(0));
	uint64_t us = amiga_now_us();

	d->secs = us / 1000000;
	d->micro = us % 1000000;
}

static const struct fdent fd_timerdev[] = {
	{ 30, "BeginIO", "a1" }, { 36, "AbortIO", "a1" },
	{ 42, "AddTime", "a0/a1" }, { 48, "SubTime", "a0/a1" },
	{ 54, "CmpTime", "a0/a1" }, { 60, "ReadEClock", "a0" },
	{ 66, "GetSysTime", "a0" },
	{ 0, 0, 0 }
};

static const struct libimpl timer_impl[] = {
	{ "BeginIO", t_BeginIO }, { "AbortIO", t_AbortIO },
	{ "AddTime", t_AddTime }, { "SubTime", t_SubTime },
	{ "CmpTime", t_CmpTime }, { "ReadEClock", t_ReadEClock },
	{ "GetSysTime", t_GetSysTime },
	{ NULL, NULL }
};

static void timer_setup(struct Library *b)
{
	TimerBase = b;
	b->lib_Node.ln_Type = 3;                /* NT_DEVICE */
}

struct amilib lib_timer = {
	.name = "timer.device", .version = 40, .revision = 1,
	.fd = fd_timerdev, .impl = timer_impl, .possize = 8,
	.init = timer_setup, .device = 1,
};

/* ---- exec's side ---- */

static int is_timer(struct ioreq *io)
{
	return io && TimerBase && io->io_Device == APTR_OF(TimerBase);
}

int timer_open(const char *name, uint32_t unit, struct ioreq *io)
{
	if (strcasecmp(name, "timer.device") || unit > UNIT_WAITECLOCK)
		return IOERR_OPENFAIL;
	lib_build(&lib_timer);
	io->io_Device = APTR_OF(TimerBase);
	io->io_Unit = unit;
	io->io_Error = 0;
	TimerBase->lib_OpenCnt++;
	return 0;
}

void timer_close(struct ioreq *io)
{
	if (is_timer(io)) {
		dequeue(io);
		TimerBase->lib_OpenCnt--;
		io->io_Device = (APTR)-1;
	}
}

static int done(struct ioreq *io)
{
	struct pending *p;

	for (p = queue; p; p = p->next)
		if (p->io == io)
			return 0;
	return 1;
}

static int wait_io(struct ioreq *io)
{
	while (!done(io)) {
		int64_t us = timer_poll();
		if (us > 0 && !done(io))
			timer_sleep(us);
	}
	/* WaitIO takes the reply off the port */
	if (io->io_Message.mn_Node.ln_Type == 7 &&
	    !(io->io_Flags & IOF_QUICK) && io->io_Message.mn_ReplyPort) {
		struct Node *n = &io->io_Message.mn_Node;
		struct MsgPort *mp = PTR(io->io_Message.mn_ReplyPort);
		struct Node *x;
		for (x = PTR(mp->mp_MsgList.lh_Head); x->ln_Succ;
		     x = PTR(x->ln_Succ))
			if (x == n) {
				((struct Node *)PTR(n->ln_Pred))->ln_Succ = n->ln_Succ;
				((struct Node *)PTR(n->ln_Succ))->ln_Pred = n->ln_Pred;
				/* the signal was consumed by WaitIO's Wait() */
				if (mp->mp_SigTask)
					((struct Task *)PTR(mp->mp_SigTask))->
						tc_SigRecvd &= ~(1u << mp->mp_SigBit);
				break;
			}
	}
	return io->io_Error;
}

void io_DoIO(struct frame *f)
{
	struct ioreq *io = PTR(A(1));

	extern int console_io(void *io);

	if (!is_timer(io)) {
		if (console_io(io)) {
			RET(io->io_Error);
			return;
		}
		af_die("%s: DoIO() on a device amigafmt does not provide",
		       af_progname);
	}
	io->io_Flags |= IOF_QUICK;
	begin_io(io);
	RET(wait_io(io));
}

void io_SendIO(struct frame *f)
{
	struct ioreq *io = PTR(A(1));

	extern int console_io(void *io);

	if (!is_timer(io)) {
		if (console_io(io)) {
			io->io_Flags &= ~IOF_QUICK;
			complete(io);
			return;
		}
		af_die("%s: SendIO() on a device amigafmt does not provide",
		       af_progname);
	}
	io->io_Flags &= ~IOF_QUICK;
	begin_io(io);
}

void io_WaitIO(struct frame *f)
{
	struct ioreq *io = PTR(A(1));

	RET(is_timer(io) ? wait_io(io) : io->io_Error);
}

void io_CheckIO(struct frame *f)
{
	struct ioreq *io = PTR(A(1));

	timer_poll();
	RET(!is_timer(io) || done(io) ? A(1) : 0);
}

void io_AbortIO(struct frame *f)
{
	struct ioreq *io = PTR(A(1));

	if (is_timer(io) && dequeue(io)) {
		io->io_Error = IOERR_ABORTED;
		complete(io);
	}
}
