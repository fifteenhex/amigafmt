/*
 * exec.library.  One task, no scheduler: Forbid/Permit and friends are
 * no-ops, signals are bits in our own Task, and Ctrl-C/D/E/F arrive as
 * SIGINT/SIGQUIT/SIGUSR1/SIGUSR2.
 */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include "af.h"

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
 * Linux's free memory as fast RAM, capped at 1 GB so programs that add
 * the figures up in 32 bits stay sane; "chip RAM" is a token 2 MB, of
 * which 1.5 MB free, because nothing here is chip RAM.
 */
static void e_AvailMem(struct frame *f)
{
	uint32_t req = D(1), fast, chip;
	struct sysinfo si;

	if (req & MEMF_TOTAL) {
		uint64_t t = 0;
		if (!sysinfo(&si))
			t = (uint64_t)si.totalram * si.mem_unit;
		fast = t > 0x40000000 ? 0x40000000 : (uint32_t)t;
		chip = 0x200000;
	} else {
		fast = amem_avail();
		if (fast > 0x40000000)
			fast = 0x40000000;
		chip = 0x180000;
	}
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

	if (!n || (me->pr_Task.tc_Node.ln_Name &&
		   !strcmp(n, PTR(me->pr_Task.tc_Node.ln_Name))))
		RET(me);
	else
		RET(0);
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

static void e_Wait(struct frame *f)
{
	struct Task *t = &me->pr_Task;
	uint32_t mask = D(0), got;

	for (;;) {
		exec_pending_break();
		got = t->tc_SigRecvd & mask;
		if (got) {
			t->tc_SigRecvd &= ~got;
			RET(got);
			return;
		}
		if (!(mask & 0xf000)) {
			dos_flush_all();
			af_die("%s: Wait($%x) for a signal nothing can send "
			       "(single task); stopping", af_progname, mask);
		}
		pause();
	}
}

static void e_Signal(struct frame *f)
{
	struct Task *t = PTR(A(1));

	t->tc_SigRecvd |= D(0);
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
	if (!A(1) || A(1) == APTR_OF(me))
		dos_exit(0);
	af_die("RemTask() of another task");
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

static void putmsg(struct MsgPort *p, struct Message *m)
{
	m->mn_Node.ln_Type = NT_MESSAGE;
	addtail(&p->mp_MsgList, &m->mn_Node);
	if ((p->mp_Flags & 3) == 0 && p->mp_SigTask)
		((struct Task *)PTR(p->mp_SigTask))->tc_SigRecvd |=
			1u << p->mp_SigBit;
}

static void e_PutMsg(struct frame *f) { putmsg(PTR(A(0)), PTR(A(1))); }

static void e_GetMsg(struct frame *f)
{
	struct MsgPort *p = PTR(A(0));
	RET(remhead(&p->mp_MsgList));
}

void exec_reply(struct Message *m)
{
	if (m->mn_ReplyPort) {
		putmsg(PTR(m->mn_ReplyPort), m);
		m->mn_Node.ln_Type = 7;         /* NT_REPLYMSG */
	} else
		m->mn_Node.ln_Type = 6;         /* NT_FREEMSG */
}

static void e_ReplyMsg(struct frame *f) { exec_reply(PTR(A(1))); }

static void e_WaitPort(struct frame *f)
{
	struct MsgPort *p = PTR(A(0));

	if (!N(p->mp_MsgList.lh_Head)->ln_Succ)
		af_die("%s: WaitPort() on an empty port would wait forever "
		       "(single task); stopping", af_progname);
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

/* no devices yet: IOERR_OPENFAIL */
static void e_OpenDevice(struct frame *f)
{
	uint8_t *io = PTR(A(1));
	int r = -1;

	if (r && af_trace)
		af_warn("OpenDevice(\"%s\") refused: no such device",
			(char *)PTR(A(0)));
	if (io)
		io[31] = r;                     /* io_Error */
	RET(r);
}

static void e_CloseDevice(struct frame *f)
{
}

/* ---- semaphores (single task: always available) ---- */

static void e_InitSemaphore(struct frame *f)
{
	uint8_t *s = PTR(A(0));

	memset(s, 0, 46);
	((struct Node *)s)->ln_Type = 15;       /* NT_SIGNALSEM */
	newlist((struct List *)(s + 16));       /* ss_WaitQueue */
}

static void e_AttemptSemaphore(struct frame *f) { RET(1); }

static void e_AddSemaphore(struct frame *f)
{
	e_InitSemaphore(f);
	enqueue(&SysBase->SemaphoreList, N(A(1) ? A(1) : A(0)));
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
		if (af_trace)
			af_warn("OpenLibrary(\"%s\", %d): not provided",
				name ? name : "(null)", (int)D(0));
		RET(0);
		return;
	}
	b = lib_build(l);
	if (D(0) > b->lib_Version) {
		RET(0);
		return;
	}
	b->lib_OpenCnt++;
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
static void e_InitStruct(struct frame *f)
{
	const uint8_t *t = PTR(A(1));
	uint8_t *base = PTR(A(2)), *dst = base;

	if (D(0))
		memset(base, 0, D(0) & 0xffff);
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
	{ "Disable", e_Nop }, { "Enable", e_Nop },
	{ "Forbid", e_Nop }, { "Permit", e_Nop },
	{ "Supervisor", e_Supervisor },
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
	{ "OpenResource", e_Zero },
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
	{ "FindResident", e_FindResident },
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
	e->AttnFlags = AFF_68010 | AFF_68020;
	if (cpu >= 68030)
		e->AttnFlags |= AFF_68030;
	if (cpu >= 68040)
		e->AttnFlags |= AFF_68040 | AFF_68881 | AFF_68882 | AFF_FPU40;
	if (cpu >= 68060)
		e->AttnFlags |= AFF_68060;
	e->VBlankFrequency = 50;
	e->PowerSupplyFrequency = 50;
	e->ex_EClockFrequency = 709379;
	e->MaxLocMem = 0x200000;
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
