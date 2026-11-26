/*
 * More than one task: exec's AddTask/RemTask and dos's CreateNewProc run
 * the new task's code on a thread of its own, but only ONE task runs at a
 * time.  A global baton (a mutex every task thread holds while it runs,
 * and a condition variable each) is handed over only in Wait() (and
 * WaitPort()), so the scheduling is cooperative and everything in
 * amigafmt stays single threaded: no library state needs a lock.  A
 * Signal()/PutMsg() from Amiga code to a task of higher priority that is
 * waiting for it hands the baton over at once, as exec would preempt.
 *
 * Threads give each task its own native (C) stack, which it needs: a task
 * blocks inside amigafmt's C code (Wait) and resumes there.  What is
 * global but belongs to the running task (native_sp, resume_frame, `me`,
 * ExecBase->ThisTask) is swapped at the handover.
 *
 * When no task can run, the one that is current sleeps for everybody in
 * exec's wait_events() (wbde's socket, timer.device, INTUITICKS, ^C), and
 * whatever that wakes is run next.
 */
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "af.h"

enum { TS_RUN, TS_READY, TS_WAIT, TS_DEAD };

struct atask {
	struct Task *t;
	pthread_t th;
	pthread_cond_t cv;
	int go, state, started;
	uint32_t initpc, finalpc, entry_d0;
	BPTR seglist;                   /* a process's code, unloaded at its end */
	void (*cstart)(struct atask *);
	/* the running task's share of the globals */
	uint32_t native_sp;
	struct frame *resume_frame;
	int dispatch_all_regs, in_supervisor;
	struct atask *next;
};

static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static struct atask *tasks, *cur, *main_task;
static int ntasks;

extern int exec_wait_events(uint32_t mask);

static struct atask *by_task(struct Task *t)
{
	struct atask *a;

	for (a = tasks; a; a = a->next)
		if (a->t == t)
			return a;
	return NULL;
}

/* the main program, as a task: called once `me` exists */
void task_init(void)
{
	struct atask *a = calloc(1, sizeof(*a));

	pthread_mutex_lock(&big);
	a->t = &me->pr_Task;
	a->th = pthread_self();
	pthread_cond_init(&a->cv, NULL);
	a->state = TS_RUN;
	a->go = 1;
	a->started = 1;
	tasks = cur = main_task = a;
	ntasks = 1;
}

int task_count(void)
{
	return ntasks;
}

struct Task *task_current(void)
{
	return cur ? cur->t : &me->pr_Task;
}

static void save(struct atask *a)
{
	a->native_sp = native_sp;
	a->resume_frame = resume_frame;
	a->dispatch_all_regs = dispatch_all_regs;
	a->in_supervisor = in_supervisor;
}

static void restore(struct atask *a)
{
	native_sp = a->native_sp;
	resume_frame = a->resume_frame;
	dispatch_all_regs = a->dispatch_all_regs;
	in_supervisor = a->in_supervisor;
	me = (struct Process *)a->t;
	SysBase->ThisTask = APTR_OF(a->t);
	cur = a;
	a->state = TS_RUN;
	a->t->tc_State = 2;                     /* TS_RUN */
}

/* hand the baton to `to` and sleep until somebody hands it back */
static void switch_to(struct atask *to)
{
	struct atask *self = cur;

	save(self);
	self->go = 0;
	to->go = 1;
	pthread_cond_signal(&to->cv);
	while (!self->go)
		pthread_cond_wait(&self->cv, &big);
	restore(self);
}

/* a task that can run: new, yielded, or waiting with its signals in */
static struct atask *pick(void)
{
	struct atask *a, *best = NULL;

	for (a = tasks; a; a = a->next) {
		if (a == cur || a->state == TS_DEAD)
			continue;
		if (a->state == TS_READY ||
		    (a->state == TS_WAIT && (a->t->tc_SigRecvd & a->t->tc_SigWait)))
			if (!best || a->t->tc_Node.ln_Pri > best->t->tc_Node.ln_Pri)
				best = a;
	}
	return best;
}

/* Wait(mask) for the running task */
uint32_t task_wait(uint32_t mask)
{
	struct Task *t;
	uint32_t got;

	for (;;) {
		exec_pending_break();
		t = cur->t;
		got = t->tc_SigRecvd & mask;
		if (got) {
			t->tc_SigRecvd &= ~got;
			return got;
		}
		if (ntasks > 1) {
			struct atask *n;
			t->tc_SigWait = mask;
			cur->state = TS_WAIT;
			t->tc_State = 4;        /* TS_WAIT */
			n = pick();
			if (n) {
				switch_to(n);
				continue;
			}
			cur->state = TS_RUN;
		}
		if (!exec_wait_events(mask)) {
			dos_flush_all();
			af_die("%s: Wait($%x) for a signal nothing can send; "
			       "stopping", af_progname, mask);
		}
	}
}

/* let another task run if one can; the caller carries on afterwards */
void task_yield(void)
{
	struct atask *n;

	if (ntasks < 2 || !(n = pick()))
		return;
	cur->state = TS_READY;
	switch_to(n);
}

/* Signal() from Amiga code: a waiting task of higher priority runs now */
void task_signal(struct Task *t, uint32_t sigs, int may_switch)
{
	struct atask *a;

	t->tc_SigRecvd |= sigs;
	if (!may_switch || ntasks < 2 || t == cur->t)
		return;
	a = by_task(t);
	if (a && a->state == TS_WAIT && (t->tc_SigRecvd & t->tc_SigWait) &&
	    t->tc_Node.ln_Pri > cur->t->tc_Node.ln_Pri) {
		cur->state = TS_READY;
		switch_to(a);
	}
}

/* the end of a task: off the list, the baton to whoever can run */
static void task_end(struct atask *a)
{
	struct atask **pp, *n;

	for (pp = &tasks; *pp; pp = &(*pp)->next)
		if (*pp == a) {
			*pp = a->next;
			break;
		}
	ntasks--;
	a->state = TS_DEAD;
	if (a != cur)
		return;                         /* it never runs again */
	save(a);
	n = pick();
	if (!n) {
		/* everybody waits: give it to the first; it sleeps in Wait()
		   for all of us, as the main task does */
		n = tasks;
	}
	if (!n)
		exit(0);
	/* the dead task's thread wakes the next and leaves */
	cur = NULL;
	n->go = 1;
	pthread_cond_signal(&n->cv);
	pthread_mutex_unlock(&big);
	pthread_exit(NULL);
}

static void *thread_main(void *arg)
{
	struct atask *a = arg;
	struct regs r;
	uint32_t ret;

	stack_t ss;

	/* the fault handler (address 4, custom chips) runs on its own stack,
	   and that is per thread */
	memset(&ss, 0, sizeof(ss));
	ss.ss_size = 65536;
	ss.ss_sp = malloc(ss.ss_size);
	sigaltstack(&ss, NULL);
	pthread_mutex_lock(&big);
	while (!a->go)
		pthread_cond_wait(&a->cv, &big);
	native_sp = 0;
	resume_frame = NULL;
	restore(a);
	if (a->state == TS_DEAD)
		task_end(a);
	a->started = 1;
	if (a->cstart)
		a->cstart(a);
	else {
		memset(&r, 0, sizeof(r));
		r.d[0] = a->entry_d0;
		r.a[6] = APTR_OF(SysBase);
		r.a[7] = a->t->tc_SPReg;
		ret = call_amiga(&r, a->initpc);
		if (a->finalpc) {
			memset(&r, 0, sizeof(r));
			r.d[0] = ret;
			r.a[6] = APTR_OF(SysBase);
			r.a[7] = a->t->tc_SPReg;
			call_amiga(&r, a->finalpc);
		}
	}
	if (af_trace)
		af_warn("task \"%s\" ended", a->t->tc_Node.ln_Name ?
			(char *)PTR(a->t->tc_Node.ln_Name) : "");
	task_end(a);
	return NULL;
}

/* start t at pc (or through cstart) with its stack in tc_SPReg; it runs
   when the current task next waits (or at once, being of higher priority,
   when may_switch) */
struct atask *task_add(struct Task *t, uint32_t initpc, uint32_t finalpc,
		       void (*cstart)(struct atask *))
{
	struct atask *a = calloc(1, sizeof(*a));
	pthread_attr_t at;

	a->t = t;
	a->initpc = initpc;
	a->finalpc = finalpc;
	a->cstart = cstart;
	a->state = TS_READY;
	pthread_cond_init(&a->cv, NULL);
	t->tc_Node.ln_Type = NT_TASK;
	t->tc_State = 3;                        /* TS_READY */
	if (!t->tc_SigAlloc)
		t->tc_SigAlloc = 0xffff;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 1 << 20);
	if (pthread_create(&a->th, &at, thread_main, a)) {
		free(a);
		return NULL;
	}
	pthread_attr_destroy(&at);
	a->next = tasks;
	tasks = a;
	ntasks++;
	if (af_trace)
		af_warn("task \"%s\" added, pc $%x", t->tc_Node.ln_Name ?
			(char *)PTR(t->tc_Node.ln_Name) : "", initpc);
	return a;
}

void task_set_d0(struct atask *a, uint32_t d0)
{
	a->entry_d0 = d0;
}

uint32_t task_initpc(struct atask *a)
{
	return a->initpc;
}

struct Task *task_of(struct atask *a)
{
	return a->t;
}

/* RemTask(t): itself ends now; another simply never runs again; the main
   program's task ending is the program ending */
void task_remove(struct Task *t)
{
	struct atask *a = t ? by_task(t) : cur;

	if (!a)
		return;
	if (a == main_task)
		dos_exit(0);
	task_end(a);
}

/* FindTask(name) over every task */
struct Task *task_find(const char *name)
{
	struct atask *a;

	for (a = tasks; a; a = a->next)
		if (a->t->tc_Node.ln_Name && !strcmp(PTR(a->t->tc_Node.ln_Name), name))
			return a->t;
	return NULL;
}
