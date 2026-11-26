/*
 * exec.library.  One task, no scheduler: Forbid/Permit and friends are
 * no-ops, signals are bits in our own Task, and Ctrl-C/D/E/F arrive as
 * SIGINT/SIGQUIT/SIGUSR1/SIGUSR2.
 */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>
#include "af.h"
#include "gui.h"

struct ExecBase *SysBase;
static volatile sig_atomic_t brk_bits;

/* ---- lists ---- */

#define N(x) ((struct Node *)PTR(x))
#define L(x) ((struct List *)PTR(x))

static void newlist(struct List *l)
{
	l->lh_Head = APTR_OF(&l->lh_Tail);
	l->lh_Tail = 0;
	l->lh_TailPred = APTR_OF(&l->lh_Head);
}

static void insert(struct List *l, struct Node *n, struct Node *pred)
{
	if (!pred) {                            /* AddHead */
		struct Node *h = N(l->lh_Head);
		n->ln_Succ = APTR_OF(h);
		n->ln_Pred = APTR_OF(&l->lh_Head);
		h->ln_Pred = APTR_OF(n);
		l->lh_Head = APTR_OF(n);
		return;
	}
	if (pred->ln_Succ == 0) {               /* pred is the list tail */
		pred = N(pred->ln_Pred);
	}
	n->ln_Succ = pred->ln_Succ;
	n->ln_Pred = APTR_OF(pred);
	N(pred->ln_Succ)->ln_Pred = APTR_OF(n);
	pred->ln_Succ = APTR_OF(n);
}

static void addtail(struct List *l, struct Node *n)
{
	struct Node *t = N(l->lh_TailPred);

	n->ln_Succ = APTR_OF(&l->lh_Tail);
	n->ln_Pred = APTR_OF(t);
	t->ln_Succ = APTR_OF(n);
	l->lh_TailPred = APTR_OF(n);
}

static void remove_node(struct Node *n)
{
	N(n->ln_Pred)->ln_Succ = n->ln_Succ;
	N(n->ln_Succ)->ln_Pred = n->ln_Pred;
}

static struct Node *remhead(struct List *l)
{
	struct Node *n = N(l->lh_Head);

	if (!n->ln_Succ)
		return NULL;
	remove_node(n);
	return n;
}

static struct Node *remtail(struct List *l)
{
	struct Node *n = N(l->lh_TailPred);

	if (!n->ln_Pred)
		return NULL;
	remove_node(n);
	return n;
}

static void enqueue(struct List *l, struct Node *n)
{
	struct Node *p;

	for (p = N(l->lh_Head); p->ln_Succ; p = N(p->ln_Succ))
		if (p->ln_Pri < n->ln_Pri)
			break;
	/* insert before p */
	n->ln_Succ = APTR_OF(p);
	n->ln_Pred = p->ln_Pred;
	N(p->ln_Pred)->ln_Succ = APTR_OF(n);
	p->ln_Pred = APTR_OF(n);
}

static struct Node *findname(struct Node *start, const char *name)
{
	struct Node *n;

	for (n = N(start->ln_Succ); n && n->ln_Succ; n = N(n->ln_Succ))
		if (n->ln_Name && !strcmp(PTR(n->ln_Name), name))
			return n;
	return NULL;
}

static void e_Insert(struct frame *f) { insert(L(A(0)), N(A(1)), A(2) ? N(A(2)) : NULL); }
static void e_AddHead(struct frame *f) { insert(L(A(0)), N(A(1)), NULL); }
static void e_AddTail(struct frame *f) { addtail(L(A(0)), N(A(1))); }
static void e_Remove(struct frame *f) { remove_node(N(A(1))); }
static void e_RemHead(struct frame *f) { RET(remhead(L(A(0)))); }
static void e_RemTail(struct frame *f) { RET(remtail(L(A(0)))); }
static void e_Enqueue(struct frame *f) { enqueue(L(A(0)), N(A(1))); }
static void e_FindName(struct frame *f) { RET(findname(N(A(0)), PTR(A(1)))); }

/* resources: the ones programs add */
static void e_AddResource(struct frame *f)
{
	struct Node *n = N(A(1));

	n->ln_Type = NT_RESOURCE;
	enqueue(&SysBase->ResourceList, n);
}

static void e_RemResource(struct frame *f) { remove_node(N(A(1))); }

static void e_OpenResource(struct frame *f)
{
	RET(findname(N(APTR_OF(&SysBase->ResourceList)), PTR(A(1))));
}

/* ---- memory ---- */

static void e_AllocMem(struct frame *f)
{
	RET(D(0) ? amem_alloc(D(0)) : NULL);
}

static void e_FreeMem(struct frame *f)
{
	if (A(1) && D(0))
		amem_free(PTR(A(1)));
}

static void e_AllocVec(struct frame *f)
{
	uint32_t *p;

	if (!D(0) || D(0) > 0x7ffffff0) {
		RET(0);
		return;
	}
	p = amem_alloc(D(0) + 8);
	if (!p) {
		RET(0);
		return;
	}
	p[1] = D(0) + 8;
	RET(p + 2);
}

static void e_FreeVec(struct frame *f)
{
	if (A(1))
		amem_free((uint32_t *)PTR(A(1)) - 2);
}

/*
 * The memory list's figures (machine.c): Linux's free memory as fast RAM,
 * and a token chip RAM, of which three quarters free, because nothing
 * here is chip RAM.
 */
static void e_AvailMem(struct frame *f)
{
	uint32_t req = D(1), fast, chip, chip_total, chip_free, fast_total,
		fast_free;

	machine_memfree(&chip_total, &chip_free, &fast_total, &fast_free);
	fast = req & MEMF_TOTAL ? fast_total : fast_free;
	chip = req & MEMF_TOTAL ? chip_total : chip_free;
	RET(req & MEMF_CHIP ? chip : req & MEMF_FAST ? fast :
	    (req & MEMF_LARGEST) ? fast : chip + fast);
}

static void e_TypeOfMem(struct frame *f)
{
	RET(MEMF_PUBLIC | MEMF_FAST);
}

static void e_CopyMem(struct frame *f)
{
	memmove(PTR(A(1)), PTR(A(0)), D(0));
}

/* struct MemList: Node, UWORD ml_NumEntries, { reqs/addr, length }[] */
static void e_AllocEntry(struct frame *f)
{
	uint8_t *ml = PTR(A(0)), *res;
	uint16_t n = *(uint16_t *)(ml + 14), i;
	uint32_t sz = 16 + 8 * n;

	res = amem_alloc(sz);
	memcpy(res, ml, sz);
	for (i = 0; i < n; i++) {
		uint32_t *me = (uint32_t *)(res + 16 + 8 * i);
		me[0] = APTR_OF(amem_alloc(me[1] ? me[1] : 1));
	}
	RET(res);
}

static void e_FreeEntry(struct frame *f)
{
	uint8_t *ml = PTR(A(0));
	uint16_t n = *(uint16_t *)(ml + 14), i;

	for (i = 0; i < n; i++)
		amem_free(PTR(*(uint32_t *)(ml + 16 + 8 * i)));
	amem_free(ml);
}

/* pools: a list of blocks, each with a two-pointer header */
struct pool {
	struct pblk *head;
};
struct pblk {
	struct pblk *next, *prev;
	uint32_t pad[2];
};

static void e_CreatePool(struct frame *f)
{
	RET(amem_alloc(sizeof(struct pool)));
}

static void e_AllocPooled(struct frame *f)
{
	struct pool *p = PTR(A(0));
	struct pblk *b;

	if (!p || !D(0)) {
		RET(0);
		return;
	}
	b = amem_alloc(D(0) + sizeof(*b));
	if (!b) {
		RET(0);
		return;
	}
	b->next = p->head;
	b->prev = NULL;
	if (p->head)
		p->head->prev = b;
	p->head = b;
	RET(b + 1);
}

static void e_FreePooled(struct frame *f)
{
	struct pool *p = PTR(A(0));
	struct pblk *b;

	if (!A(1))
		return;
	b = (struct pblk *)PTR(A(1)) - 1;
	if (b->prev)
		b->prev->next = b->next;
	else
		p->head = b->next;
	if (b->next)
		b->next->prev = b->prev;
	amem_free(b);
}

static void e_DeletePool(struct frame *f)
{
	struct pool *p = PTR(A(0));
	struct pblk *b, *n;

	if (!p)
		return;
	for (b = p->head; b; b = n) {
		n = b->next;
		amem_free(b);
	}
	amem_free(p);
}

/* ---- tasks and signals ---- */

static int in_interrupt;         /* interrupt code: no task switches */

extern uint32_t task_wait(uint32_t mask);
extern void task_signal(struct Task *t, uint32_t sigs, int may_switch);
extern struct Task *task_current(void), *task_find(const char *name);
extern void task_remove(struct Task *t);
struct atask;
extern struct atask *task_add(struct Task *t, uint32_t initpc, uint32_t finalpc,
			      void (*cstart)(struct atask *));

static void on_signal(int sig)
{
	uint32_t bit = sig == SIGINT ? SIGBREAKF_CTRL_C :
		       sig == SIGQUIT ? SIGBREAKF_CTRL_D :
		       sig == SIGUSR1 ? SIGBREAKF_CTRL_E : SIGBREAKF_CTRL_F;
	struct Task *t = &me->pr_Task;

	/* a program that never looks for ^C still has to die on the second */
	if (sig == SIGINT && ((brk_bits | t->tc_SigRecvd) & SIGBREAKF_CTRL_C)) {
		signal(SIGINT, SIG_DFL);
		raise(SIGINT);
	}
	brk_bits |= bit;
}

void exec_pending_break(void)
{
	if (brk_bits && me) {
		me->pr_Task.tc_SigRecvd |= brk_bits;
		brk_bits = 0;
	}
}

static void e_FindTask(struct frame *f)
{
	const char *n = PTR(A(1));

	RET(n ? task_find(n) : task_current());
}

static void e_SetTaskPri(struct frame *f)
{
	struct Task *t = PTR(A(1));
	int8_t old = t->tc_Node.ln_Pri;

	t->tc_Node.ln_Pri = D(0);
	RET((int32_t)old);
}

static void e_SetSignal(struct frame *f)
{
	struct Task *t = &me->pr_Task;
	uint32_t old;

	exec_pending_break();
	old = t->tc_SigRecvd;
	t->tc_SigRecvd = (old & ~D(1)) | (D(0) & D(1));
	RET(old);
}

static void e_SetExcept(struct frame *f)
{
	struct Task *t = &me->pr_Task;
	uint32_t old = t->tc_SigExcept;

	t->tc_SigExcept = (old & ~D(1)) | (D(0) & D(1));
	RET(old);
}

/*
 * Sleep until something may have changed: a timer.device request falls
 * due, wbde sends something (events become IntuiMessages, so port
 * signals), INTUITICKS are due, or a ^C.  0 when nothing can ever wake us.
 */
extern int intui_wants_ticks(void);
extern void intui_ticks(void);

static int64_t mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t native_vblank_sp(void)
{
	static uint8_t *stack;

	if (!stack)
		stack = amem_alloc(16384);
	return APTR_OF(stack + 16384 - 16);
}

/*
 * Interrupt servers (AddIntServer): the vertical blank ones (INTB_VERTB,
 * 5) run 50 times a second while the program waits, with a1 = is_Data,
 * a5 = is_Code, a6 = SysBase, as exec calls them; the others are kept
 * and never called (there are no such interrupts here).
 */
#define MAXINTS 16
static struct { int num; uint32_t is; } ints[MAXINTS];
static int nints, nvertb;
static int64_t last_vblank;

static void e_AddIntServer(struct frame *f)
{
	if (nints < MAXINTS) {
		ints[nints].num = D(0);
		ints[nints].is = A(1);
		nints++;
		nvertb += D(0) == 5;
	}
}

static void e_RemIntServer(struct frame *f)
{
	int i;

	for (i = 0; i < nints; i++)
		if (ints[i].is == A(1) && ints[i].num == (int)D(0)) {
			nvertb -= ints[i].num == 5;
			ints[i] = ints[--nints];
			return;
		}
}

/* run the vertical blank servers if one is due; microseconds to the next */
static int64_t vblank(void)
{
	int64_t now = mono_us();
	int i;

	if (!nvertb)
		return -1;
	if (now - last_vblank >= 20000) {
		last_vblank = now;
		for (i = 0; i < nints; i++)
			if (ints[i].num == 5) {
				uint8_t *is = PTR(ints[i].is);
				struct regs r;
				memset(&r, 0, sizeof(r));
				r.a[1] = *(uint32_t *)(is + 14);
				r.a[5] = *(uint32_t *)(is + 18);
				r.a[6] = APTR_OF(SysBase);
				r.a[7] = native_vblank_sp();
				in_interrupt++;
				call_amiga(&r, r.a[5]);
				in_interrupt--;
			}
		now = mono_us();
	}
	return 20000 - (now - last_vblank);
}

/*
 * Linux file descriptors a task waits on (bsdsocket's WaitSelect and its
 * blocking calls): whoever sleeps for everybody in exec_wait_events() or
 * exec_sleep() polls them too, and signals the waiting task when one is
 * ready or its deadline passes.  A watcher with fd_fire set instead calls
 * that (asynchronous socket notification).
 */
static struct exec_fdwait *fdwaits;

void exec_fdwait_add(struct exec_fdwait *w)
{
	w->next = fdwaits;
	fdwaits = w;
}

void exec_fdwait_remove(struct exec_fdwait *w)
{
	struct exec_fdwait **pp;

	for (pp = &fdwaits; *pp; pp = &(*pp)->next)
		if (*pp == w) {
			*pp = w->next;
			return;
		}
}

int64_t exec_now_us(void)
{
	return mono_us();
}

/* microseconds to the earliest watcher deadline, -1 for none */
static int64_t fdwait_due(void)
{
	struct exec_fdwait *w;
	int64_t now = mono_us(), best = -1;

	for (w = fdwaits; w; w = w->next)
		if (w->deadline >= 0) {
			int64_t d = w->deadline - now;
			if (d < 0)
				d = 0;
			if (best < 0 || d < best)
				best = d;
		}
	return best;
}

/*
 * Poll wbde's socket and every watcher's descriptors for up to us
 * microseconds (-1: for ever).  Wakes the watchers that are ready or due;
 * 1 when anything happened (an event, a watcher, a Linux signal).
 */
static int poll_all(int64_t us)
{
	struct pollfd pf[64];
	struct exec_fdwait *w, *next;
	int n = 0, gi = -1, fd = gui_fd(), r, any = 0;
	int64_t due = fdwait_due(), now;

	if (due >= 0 && (us < 0 || due < us))
		us = due;
	if (fd >= 0) {
		gi = n;
		pf[n++] = (struct pollfd){ fd, POLLIN, 0 };
	}
	for (w = fdwaits; w; w = w->next) {
		w->first = n;
		for (int i = 0; i < w->n && n < 64; i++)
			pf[n++] = w->pfd[i];
		w->last = n;
	}
	r = poll(pf, n, us < 0 ? -1 : (int)((us + 999) / 1000));
	if (r < 0)
		return 1;                       /* a Linux signal (^C) */
	if (gi >= 0 && pf[gi].revents) {
		gui_pump(0);
		any = 1;
	}
	now = mono_us();
	for (w = fdwaits; w; w = next) {
		int ready = 0;
		next = w->next;
		for (int i = w->first; i < w->last; i++)
			if (pf[i].revents) {
				w->pfd[i - w->first].revents = pf[i].revents;
				ready = 1;
			}
		if (ready || (w->deadline >= 0 && now >= w->deadline)) {
			w->fired = 1;
			any = 1;
			if (w->fire)
				w->fire(w);
			else if (w->task)
				task_signal(w->task, w->sig, 0);
		}
	}
	return any || r > 0;
}

int exec_have_fdwaits(void)
{
	return fdwaits != NULL;
}

/*
 * Sleep for us microseconds the way a task blocked in WaitTOF() or
 * Delay() does: other tasks run meanwhile (a program that polls another
 * task with WaitTOF in a loop needs that), wbde's events and timer.device
 * keep being served, vertical blank servers keep running.
 */
void exec_sleep(int64_t us)
{
	extern void task_yield(void);
	int64_t end = mono_us() + us, left;

	for (;;) {
		task_yield();
		left = end - mono_us();
		if (left <= 0)
			break;
		gui_flush();
		dos_flush_all();
		{
			int64_t vb = vblank(), t = timer_poll();
			int fd = gui_fd();

			if (vb >= 0 && vb < left)
				left = vb > 0 ? vb : 1;
			if (t >= 0 && t < left)
				left = t > 0 ? t : 1;
			if (fd >= 0 || fdwaits)
				poll_all(left);
			else
				timer_sleep(left);
			timer_poll();
		}
	}
}

int exec_wait_events(uint32_t mask)
{
	static int64_t last_tick;
	int64_t us = timer_poll(), now, vb;
	int ticks = intui_wants_ticks(), fd = gui_fd();

	gui_flush();
	dos_flush_all();
	if (us == 0)
		return 1;
	vb = vblank();
	if (vb >= 0 && (us < 0 || us > vb))
		us = vb > 0 ? vb : 1;
	if (ticks) {
		now = mono_us();
		if (now - last_tick >= 100000) {
			last_tick = now;
			intui_ticks();
			return 1;
		}
		if (us < 0 || us > 100000 - (now - last_tick))
			us = 100000 - (now - last_tick);
	}
	if (fd >= 0 || fdwaits) {
		poll_all(us);
		timer_poll();
		return 1;
	}
	if (us >= 0) {
		timer_sleep(us);
		timer_poll();
		return 1;
	}
	if (mask & 0xf000) {
		pause();
		return 1;
	}
	return 0;
}


static void e_Wait(struct frame *f)
{
	RET(task_wait(D(0)));
}

static void e_Signal(struct frame *f)
{
	task_signal(PTR(A(1)), D(0), !in_interrupt);
}

static void e_AddTask(struct frame *f)
{
	struct Task *t = PTR(A(1));

	if (!task_add(t, A(2), A(3), NULL)) {
		RET(0);
		return;
	}
	RET(t);
}

static void e_AllocSignal(struct frame *f)
{
	struct Task *t = &me->pr_Task;
	int32_t n = (int8_t)D(0);

	if (n == -1) {
		for (n = 31; n >= 16; n--)
			if (!(t->tc_SigAlloc & (1u << n)))
				break;
		if (n < 16) {
			RET(-1);
			return;
		}
	} else if (n < 0 || n > 31 || (t->tc_SigAlloc & (1u << n))) {
		RET(-1);
		return;
	}
	t->tc_SigAlloc |= 1u << n;
	t->tc_SigRecvd &= ~(1u << n);
	RET(n);
}

static void e_FreeSignal(struct frame *f)
{
	int32_t n = D(0);

	if (n >= 0 && n < 32)
		me->pr_Task.tc_SigAlloc &= ~(1u << n);
}

static void e_AllocTrap(struct frame *f) { RET(-1); }
static void e_Nop(struct frame *f) { (void)f; }
static void e_Zero(struct frame *f) { RET(0); }

static void e_RemTask(struct frame *f)
{
	task_remove(A(1) ? PTR(A(1)) : NULL);
}

static void e_Alert(struct frame *f)
{
	dos_flush_all();
	fprintf(stderr, "amigafmt: %s: Alert $%08x\n", af_progname, D(7));
	if (D(7) & 0x80000000)
		exit(20);
}

/*
 * Supervisor(): run the function in user mode on a fake exception frame;
 * the SIGILL handler emulates the privileged instructions such code uses
 * (MOVEC reads, MOVE SR, cache pushes, FSAVE/FRESTORE) and its RTE.
 * Everything the function leaves in the registers goes back to the caller.
 */
int in_supervisor;

static void e_Supervisor(struct frame *f)
{
	struct regs r;

	memcpy(r.d, f->d, sizeof(r.d));
	memcpy(r.a, f->a, 7 * 4);
	in_supervisor++;
	amiga_call_super(f, A(5), &r);
	in_supervisor--;
	memcpy(f->d, r.d, sizeof(r.d));
	memcpy(f->a, r.a, 7 * 4);
	dispatch_all_regs = 1;
}

/* ---- ports and messages ---- */

void exec_putmsg(struct MsgPort *p, struct Message *m);
static void putmsg(struct MsgPort *p, struct Message *m)
{
	exec_putmsg(p, m);
}

void exec_putmsg(struct MsgPort *p, struct Message *m)
{
	m->mn_Node.ln_Type = NT_MESSAGE;
	addtail(&p->mp_MsgList, &m->mn_Node);
	if ((p->mp_Flags & 3) == 0 && p->mp_SigTask)
		task_signal(PTR(p->mp_SigTask), 1u << p->mp_SigBit, 0);
}

/* after a message from Amiga code: its receiver may be due to run */
static void msg_sent(struct MsgPort *p)
{
	if (in_interrupt)
		return;
	if (p && (p->mp_Flags & 3) == 0 && p->mp_SigTask)
		task_signal(PTR(p->mp_SigTask), 0, 1);
}

static void e_PutMsg(struct frame *f)
{
	putmsg(PTR(A(0)), PTR(A(1)));
	msg_sent(PTR(A(0)));
}

/* a program polling its window's port still gets wbde's events */
struct Node *exec_getmsg(struct MsgPort *p)
{
	if (!N(p->mp_MsgList.lh_Head)->ln_Succ && gui_fd() >= 0) {
		gui_flush();
		gui_pump(0);
	}
	return remhead(&p->mp_MsgList);
}

static void e_GetMsg(struct frame *f)
{
	RET(exec_getmsg(PTR(A(0))));
}

extern int intui_reply(struct Message *m);

void exec_reply(struct Message *m)
{
	if (intui_reply(m))
		return;
	if (m->mn_ReplyPort) {
		putmsg(PTR(m->mn_ReplyPort), m);
		m->mn_Node.ln_Type = 7;         /* NT_REPLYMSG */
	} else
		m->mn_Node.ln_Type = 6;         /* NT_FREEMSG */
}

static void e_ReplyMsg(struct frame *f)
{
	struct Message *m = PTR(A(1));
	uint32_t port = m ? m->mn_ReplyPort : 0;

	exec_reply(m);
	if (port)
		msg_sent(PTR(port));
}

static void e_WaitPort(struct frame *f)
{
	struct MsgPort *p = PTR(A(0));

	while (!N(p->mp_MsgList.lh_Head)->ln_Succ) {
		exec_pending_break();
		if ((p->mp_Flags & 3) == 0 && p->mp_SigTask == APTR_OF(task_current()))
			task_wait(1u << p->mp_SigBit);
		else if (!exec_wait_events(0))
			af_die("%s: WaitPort() on an empty port would wait "
			       "forever; stopping", af_progname);
	}
	RET(p->mp_MsgList.lh_Head);
}

static void e_AddPort(struct frame *f)
{
	struct MsgPort *p = PTR(A(1));

	newlist(&p->mp_MsgList);
	enqueue(&SysBase->PortList, &p->mp_Node);
}

static void e_RemPort(struct frame *f) { remove_node(N(A(1))); }

static void e_FindPort(struct frame *f)
{
	RET(findname((struct Node *)&SysBase->PortList, PTR(A(1))));
}

static void e_CreateMsgPort(struct frame *f)
{
	struct MsgPort *p = amem_alloc(sizeof(*p));
	struct frame g = *f;

	g.d[0] = (uint32_t)-1;
	e_AllocSignal(&g);
	if ((int32_t)g.d[0] < 0) {
		amem_free(p);
		RET(0);
		return;
	}
	p->mp_Node.ln_Type = NT_MSGPORT;
	p->mp_SigBit = g.d[0];
	p->mp_SigTask = APTR_OF(me);
	newlist(&p->mp_MsgList);
	RET(p);
}

static void e_DeleteMsgPort(struct frame *f)
{
	struct MsgPort *p = PTR(A(0));

	if (!p)
		return;
	me->pr_Task.tc_SigAlloc &= ~(1u << p->mp_SigBit);
	amem_free(p);
}

static void e_CreateIORequest(struct frame *f)
{
	uint8_t *io;

	if (!A(0) || D(0) < sizeof(struct Message)) {
		RET(0);
		return;
	}
	io = amem_alloc(D(0));
	((struct Message *)io)->mn_ReplyPort = A(0);
	((struct Message *)io)->mn_Length = D(0);
	RET(io);
}

static void e_DeleteIORequest(struct frame *f)
{
	amem_free(PTR(A(0)));
}

/* timer.device is the only device; anything else: IOERR_OPENFAIL */
extern int timer_open(const char *name, uint32_t unit, void *io);
extern void timer_close(void *io);
extern void io_DoIO(struct frame *f), io_SendIO(struct frame *f),
	io_WaitIO(struct frame *f), io_CheckIO(struct frame *f),
	io_AbortIO(struct frame *f);

static void e_OpenDevice(struct frame *f)
{
	uint8_t *io = PTR(A(1));
	int r = io ? timer_open(PTR(A(0)), D(0), io) : -1;

	if (r && io && !strcasecmp(PTR(A(0)), "input.device")) {
		*(uint32_t *)(io + 20) = APTR_OF(lib_build(&lib_input));
		r = 0;
	}
	if (r && io && !strcasecmp(PTR(A(0)), "console.device")) {
		/* CONU_LIBRARY, or a unit on a window (writes only) */
		*(uint32_t *)(io + 20) = APTR_OF(lib_build(&lib_console));
		r = 0;
	}

	if (r && af_trace)
		af_warn("OpenDevice(\"%s\") refused: no such device",
			(char *)PTR(A(0)));
	if (io)
		io[31] = r;                     /* io_Error */
	RET(r);
}

static void e_CloseDevice(struct frame *f)
{
	if (A(1))
		timer_close(PTR(A(1)));
}

/* ---- semaphores (single task: always available) ---- */

/* the node's name and priority are the caller's (AddSemaphore needs them) */
static void init_semaphore(uint8_t *s)
{
	memset(s + 14, 0, 46 - 14);
	((struct Node *)s)->ln_Type = 15;       /* NT_SIGNALSEM */
	newlist((struct List *)(s + 16));       /* ss_WaitQueue */
	*(int16_t *)(s + 44) = -1;              /* ss_QueueCount */
}

static void e_InitSemaphore(struct frame *f) { init_semaphore(PTR(A(0))); }

static void e_AttemptSemaphore(struct frame *f) { RET(1); }

static void e_AddSemaphore(struct frame *f)
{
	init_semaphore(PTR(A(1)));
	enqueue(&SysBase->SemaphoreList, N(A(1)));
}

static void e_FindSemaphore(struct frame *f)
{
	RET(findname((struct Node *)&SysBase->SemaphoreList, PTR(A(1))));
}

/* ---- libraries ---- */

static void e_OpenLibrary(struct frame *f)
{
	const char *name = PTR(A(1));
	struct amilib *l = name ? lib_find(name) : NULL;
	struct Library *b;

	if (!l) {
		extern uint32_t ramlib_open(struct frame *f, const char *name,
					    uint32_t version);
		uint32_t r = name ? ramlib_open(f, name, D(0)) : 0;
		if (!r && af_trace)
			af_warn("OpenLibrary(\"%s\", %d): not provided",
				name ? name : "(null)", (int)D(0));
		RET(r);
		return;
	}
	b = lib_build(l);
	if (D(0) > b->lib_Version) {
		RET(0);
		return;
	}
	b->lib_OpenCnt++;
	if (l->open)
		b = l->open(l);
	RET(b);
}

static void e_OldOpenLibrary(struct frame *f)
{
	D(0) = 0;
	e_OpenLibrary(f);
}

static void e_CloseLibrary(struct frame *f)
{
	struct Library *b = PTR(A(1));
	extern int ramlib_close(struct frame *f, struct Library *lib);

	if (ramlib_close(f, b))
		return;
	for (int i = 0; b && all_libs[i]; i++)
		if (all_libs[i]->close && all_libs[i]->close(b)) {
			struct Library *t = all_libs[i]->base;
			if (t && t->lib_OpenCnt)
				t->lib_OpenCnt--;
			return;
		}
	if (b && b->lib_OpenCnt)
		b->lib_OpenCnt--;
}

static void e_SetFunction(struct frame *f)
{
	uint8_t *v = (uint8_t *)PTR(A(1)) + (int16_t)A(0);
	uint32_t old = (uint32_t)v[2] << 24 | v[3] << 16 | v[4] << 8 | v[5];

	v[0] = 0x4e, v[1] = 0xf9;
	v[2] = D(0) >> 24, v[3] = D(0) >> 16, v[4] = D(0) >> 8, v[5] = D(0);
	cache_flush(v, 6);
	RET(old);
}

static void e_SumLibrary(struct frame *f) { (void)f; }

/* V39's private TaggedOpenLibrary(tag): the ROM's own table of names */
static void e_TaggedOpenLibrary(struct frame *f)
{
	static const char *names[] = { NULL, "graphics.library",
		"layers.library", "intuition.library", "dos.library",
		"icon.library", "expansion.library", "utility.library",
		"keymap.library", "gadtools.library", "workbench.library" };
	int32_t t = D(0);

	if (t < 1 || t > 10) {
		RET(0);
		return;
	}
	A(1) = APTR_OF(names[t]);
	D(0) = 0;
	e_OpenLibrary(f);
}

/* InitStruct(initTable, memory, size): the exec.library table format */
void exec_initstruct(const uint8_t *t, uint8_t *base, uint32_t size)
{
	uint8_t *dst = base;

	if (size)
		memset(base, 0, size & 0xffff);
	for (;;) {
		uint8_t cmd = *t++;
		int what = cmd >> 6, size = (cmd >> 4) & 3, cnt = (cmd & 15) + 1;
		int sz = size == 0 ? 4 : size == 1 ? 2 : 1;
		if (!cmd)
			break;
		if (size == 3)
			af_die("InitStruct: bad size in table");
		if (what == 2) {                /* byte offset */
			dst = base + *t++;
		} else if (what == 3) {         /* 24-bit offset */
			dst = base + (t[0] << 16 | t[1] << 8 | t[2]);
			t += 3;
		}
		if (sz > 1 && ((uintptr_t)t & 1))
			t++;
		if (what == 1) {                /* repeat one value */
			for (int i = 0; i < cnt; i++)
				memcpy(dst + i * sz, t, sz);
			dst += cnt * sz;
			t += sz;
		} else {                        /* copy cnt values */
			memcpy(dst, t, cnt * sz);
			dst += cnt * sz;
			t += cnt * sz;
		}
		if ((uintptr_t)t & 1)
			t++;
	}
}

static void e_InitStruct(struct frame *f)
{
	exec_initstruct(PTR(A(1)), PTR(A(2)), D(0));
}

/* ---- misc ---- */

static void e_CacheClearU(struct frame *f) { cache_flush(0, 0xffffffff); }

static void e_CacheClearE(struct frame *f)
{
	cache_flush(PTR(A(0)), D(0));
}

static void e_CacheControl(struct frame *f)
{
	static uint32_t cacr = 0x80000101;      /* CopyBack|EnableD|EnableI */
	uint32_t old = cacr;

	cacr = (cacr & ~D(1)) | (D(0) & D(1));
	RET(old);
}

static void e_CachePreDMA(struct frame *f) { RET(A(0)); }

static void e_RawDoFmt_put(int c, void *ctx);

struct rdf_ctx {
	struct frame *f;
	uint32_t putch, data;
	struct regs r;
};

static void e_RawDoFmt_put(int c, void *ctx)
{
	struct rdf_ctx *x = ctx;

	if (!x->putch)
		return;
	/* the usual "move.b d0,(a3)+ ; rts" needs no trip into 68k code */
	if (*(uint32_t *)PTR(x->putch) == 0x16c04e75) {
		*(uint8_t *)PTR(x->r.a[3]) = c;
		x->r.a[3]++;
		return;
	}
	x->r.d[0] = c;
	amiga_call(x->f, x->putch, &x->r);
}

static void e_RawDoFmt(struct frame *f)
{
	struct rdf_ctx x;
	uint32_t next;

	memset(&x, 0, sizeof(x));
	x.f = f;
	x.putch = A(2);
	x.r.a[3] = A(3);
	x.r.a[6] = APTR_OF(SysBase);
	next = fmt_do(PTR(A(0)), A(1), e_RawDoFmt_put, &x, 0);
	e_RawDoFmt_put(0, &x);
	RET(next);
}

static void e_RawPutChar(struct frame *f)
{
	char c = D(0);

	if (write(2, &c, 1) < 0) {
		/* nowhere to report it */
	}
}

static void e_RawMayGetChar(struct frame *f) { RET(-1); }

static void e_FindResident(struct frame *f) { RET(0); }
extern void e_MakeLibrary(struct frame *f), e_MakeFunctions(struct frame *f),
	e_InitResident(struct frame *f), e_AddLibrary(struct frame *f),
	e_RemLibrary(struct frame *f);

static void e_ColdReboot(struct frame *f)
{
	af_die("%s: ColdReboot() refused", af_progname);
}

static void e_StackSwap(struct frame *f)
{
	/* struct StackSwapStruct { stk_Lower, stk_Upper, stk_Pointer } */
	uint32_t *ss = PTR(A(0));
	struct Task *t = &me->pr_Task;
	struct frame *nf;
	uint32_t oldsp = (uint32_t)(uintptr_t)&f->ret + 4;
	uint32_t lo = t->tc_SPLower, hi = t->tc_SPUpper;

	nf = (struct frame *)((uint8_t *)PTR(ss[2]) - sizeof(*nf));
	*nf = *f;
	t->tc_SPLower = ss[0];
	t->tc_SPUpper = ss[1];
	ss[0] = lo;
	ss[1] = hi;
	ss[2] = oldsp;
	resume_frame = nf;
}

static const struct libimpl exec_impl[] = {
	{ "Supervisor", e_Supervisor },
	{ "Disable", e_Nop }, { "Enable", e_Nop },
	{ "Forbid", e_Nop }, { "Permit", e_Nop },
	{ "SetSR", e_Zero }, { "SuperState", e_Zero }, { "UserState", e_Nop },
	{ "GetCC", e_Zero }, { "Debug", e_Nop }, { "Alert", e_Alert },
	{ "Insert", e_Insert }, { "AddHead", e_AddHead },
	{ "AddTail", e_AddTail }, { "Remove", e_Remove },
	{ "RemHead", e_RemHead }, { "RemTail", e_RemTail },
	{ "Enqueue", e_Enqueue }, { "FindName", e_FindName },
	{ "AllocMem", e_AllocMem }, { "FreeMem", e_FreeMem },
	{ "AllocAbs", e_Zero }, { "AvailMem", e_AvailMem },
	{ "AllocEntry", e_AllocEntry }, { "FreeEntry", e_FreeEntry },
	{ "AllocVec", e_AllocVec }, { "FreeVec", e_FreeVec },
	{ "TypeOfMem", e_TypeOfMem },
	{ "CopyMem", e_CopyMem }, { "CopyMemQuick", e_CopyMem },
	{ "CreatePool", e_CreatePool }, { "DeletePool", e_DeletePool },
	{ "AllocPooled", e_AllocPooled }, { "FreePooled", e_FreePooled },
	{ "FindTask", e_FindTask }, { "SetTaskPri", e_SetTaskPri },
	{ "RemTask", e_RemTask },
	{ "SetSignal", e_SetSignal }, { "SetExcept", e_SetExcept },
	{ "Wait", e_Wait }, { "Signal", e_Signal },
	{ "AllocSignal", e_AllocSignal }, { "FreeSignal", e_FreeSignal },
	{ "AllocTrap", e_AllocTrap }, { "FreeTrap", e_Nop },
	{ "AddPort", e_AddPort }, { "RemPort", e_RemPort },
	{ "PutMsg", e_PutMsg }, { "GetMsg", e_GetMsg },
	{ "ReplyMsg", e_ReplyMsg }, { "WaitPort", e_WaitPort },
	{ "FindPort", e_FindPort },
	{ "CreateMsgPort", e_CreateMsgPort },
	{ "DeleteMsgPort", e_DeleteMsgPort },
	{ "CreateIORequest", e_CreateIORequest },
	{ "DeleteIORequest", e_DeleteIORequest },
	{ "OpenDevice", e_OpenDevice }, { "CloseDevice", e_CloseDevice },
	{ "DoIO", io_DoIO }, { "SendIO", io_SendIO }, { "WaitIO", io_WaitIO },
	{ "CheckIO", io_CheckIO }, { "AbortIO", io_AbortIO },
	{ "OpenResource", e_OpenResource }, { "AddResource", e_AddResource },
	{ "RemResource", e_RemResource },
	{ "InitSemaphore", e_InitSemaphore },
	{ "ObtainSemaphore", e_Nop }, { "ReleaseSemaphore", e_Nop },
	{ "AttemptSemaphore", e_AttemptSemaphore },
	{ "ObtainSemaphoreShared", e_Nop },
	{ "AttemptSemaphoreShared", e_AttemptSemaphore },
	{ "ObtainSemaphoreList", e_Nop }, { "ReleaseSemaphoreList", e_Nop },
	{ "AddSemaphore", e_AddSemaphore }, { "RemSemaphore", e_Remove },
	{ "FindSemaphore", e_FindSemaphore },
	{ "Procure", e_AttemptSemaphore }, { "Vacate", e_Nop },
	{ "OldOpenLibrary", e_OldOpenLibrary },
	{ "OpenLibrary", e_OpenLibrary }, { "CloseLibrary", e_CloseLibrary },
	{ "SetFunction", e_SetFunction }, { "SumLibrary", e_SumLibrary },
	{ "TaggedOpenLibrary", e_TaggedOpenLibrary },
	{ "InitStruct", e_InitStruct },
	{ "CacheClearU", e_CacheClearU }, { "CacheClearE", e_CacheClearE },
	{ "CacheControl", e_CacheControl },
	{ "CachePreDMA", e_CachePreDMA }, { "CachePostDMA", e_Nop },
	{ "RawDoFmt", e_RawDoFmt },
	/* private in the .fd: RawIOInit -504, RawMayGetChar -510,
	   RawPutChar -516 (exec/exec_lib.fd's execPrivate7..9) */
	{ "execPrivate7", e_Nop }, { "execPrivate8", e_RawMayGetChar },
	{ "execPrivate9", e_RawPutChar },
	{ "FindResident", e_FindResident }, { "AddTask", e_AddTask },
	{ "AddIntServer", e_AddIntServer }, { "RemIntServer", e_RemIntServer },
	{ "MakeLibrary", e_MakeLibrary }, { "MakeFunctions", e_MakeFunctions },
	{ "InitResident", e_InitResident }, { "AddLibrary", e_AddLibrary },
	{ "RemLibrary", e_RemLibrary },
	{ "ColdReboot", e_ColdReboot },
	{ "StackSwap", e_StackSwap },
	{ NULL, NULL }
};

static int cpu_model(void)
{
	FILE *fp = fopen("/proc/cpuinfo", "r");
	char line[256];
	int m = 68060;

	if (!fp)
		return m;
	while (fgets(line, sizeof(line), fp))
		if (!strncmp(line, "CPU:", 4)) {
			char *p = strstr(line, "680");
			if (p)
				m = atoi(p);
		}
	fclose(fp);
	return m;
}

static void exec_setup(struct Library *b)
{
	struct ExecBase *e = (struct ExecBase *)b;
	int cpu = cpu_model();

	SysBase = e;
	e->SoftVer = b->lib_Revision;
	newlist(&e->MemList);
	newlist(&e->ResourceList);
	newlist(&e->DeviceList);
	newlist(&e->IntrList);
	newlist(&e->LibList);
	newlist(&e->PortList);
	newlist(&e->TaskReady);
	newlist(&e->TaskWait);
	newlist(&e->SemaphoreList);
	e->LibList.lh_Type = NT_LIBRARY;
	e->MemList.lh_Type = NT_MEMORY;
	e->ResourceList.lh_Type = NT_RESOURCE;
	e->DeviceList.lh_Type = NT_DEVICE;
	e->PortList.lh_Type = NT_MSGPORT;
	e->AttnFlags = AFF_68010 | AFF_68020;
	if (cpu >= 68030)
		e->AttnFlags |= AFF_68030;
	if (cpu >= 68040)
		e->AttnFlags |= AFF_68040 | AFF_68881 | AFF_68882 | AFF_FPU40;
	if (cpu >= 68060)
		e->AttnFlags |= AFF_68060;
	machine_init();
	e->VBlankFrequency = machine.pal ? 50 : 60;
	e->PowerSupplyFrequency = machine.psfreq;
	e->ex_EClockFrequency = machine.eclock;
	e->MaxLocMem = machine.chip_size;
	machine_memlist(e);
	e->Quantum = 4;
}

struct amilib lib_exec = {
	.name = "exec.library", .version = 40, .revision = 68,
	.fd = fd_exec, .impl = exec_impl,
	.possize = sizeof(struct ExecBase) - sizeof(struct Library),
	.init = exec_setup,
};

void exec_init(void)
{
	struct sigaction sa;
	int i;

	lib_build(&lib_exec);
	for (i = 0; all_libs[i]; i++)
		if (!all_libs[i]->hidden)
			addtail(all_libs[i]->device ? &SysBase->DeviceList :
				&SysBase->LibList, &lib_build(all_libs[i])->lib_Node);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
}

/* used by dos for NewList-style setup */
void exec_newlist(void *l) { newlist(l); }
void exec_addtail(void *l, void *n) { addtail(l, n); }
void exec_enqueue(void *l, void *n) { enqueue(l, n); }
void exec_remove(void *n) { remove_node(n); }
