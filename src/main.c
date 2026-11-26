/*
 * amigafmt: run an AmigaOS hunk executable on m68k Linux.
 *
 *   amigafmt [options] program [arguments...]
 *
 * Registered with binfmt_misc on the hunk magic, `./program args` ends up
 * here as `amigafmt /path/to/program args`.
 *
 * ExecBase must be at address 4.  As root (CAP_SYS_RAWIO) page 0 can be
 * mapped below vm.mmap_min_addr; otherwise, or under qemu-m68k, reads of
 * address 4 fault and the SIGSEGV handler emulates the instruction.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include "gui.h"

int af_trace;
const char *af_progname = "amigafmt";
static BPTR the_seg;
static int page0;
static uint32_t stack_lo, stack_hi;

/* ---- reads of memory that is not there: emulate the instruction ----
 *
 * Without page 0 (not root, or qemu-m68k) a read of address 4 faults;
 * so does any read of the custom chips, which programs like the 3.1.4
 * C:CPU do to identify the chipset.  The SIGSEGV handler decodes MOVE
 * (byte, word, long; source (An), (An)+, d16(An), abs.w, abs.l; any
 * simple destination) and supplies the value.
 */
static uint32_t *areg(greg_t *g, int n) { return (uint32_t *)&g[R_A0 + n]; }

/* effective address of a source operand; returns its length in words */
static int src_ea(greg_t *g, const uint16_t *ext, int mode, int reg,
		  uint32_t *ea)
{
	switch (mode) {
	case 2:                                 /* (An) */
	case 3:                                 /* (An)+ */
		*ea = *areg(g, reg);
		return 0;
	case 5:                                 /* d16(An) */
		*ea = *areg(g, reg) + (int16_t)ext[0];
		return 1;
	case 7:
		if (reg == 0) {                 /* abs.w */
			*ea = (int16_t)ext[0];
			return 1;
		}
		if (reg == 1) {                 /* abs.l */
			*ea = (uint32_t)ext[0] << 16 | ext[1];
			return 2;
		}
	}
	return -1;
}

/* custom chip registers a program may read: an AGA machine */
static uint16_t custom_read(uint32_t off)
{
	switch (off & 0x1fe) {
	case 0x000: {                           /* BLTDDAT */
		extern uint16_t blitter_read(uint32_t off);
		return blitter_read(off);
	}
	case 0x002: return 0x03f0;              /* DMACONR: the blitter is never busy */
	case 0x004: return machine_vposr();     /* VPOSR: Agnus/Alice ID */
	case 0x07c: return machine_deniseid();  /* DENISEID */
	default: return 0;
	}
}

/* the value of [addr, addr+size) if it is something we stand in for */
static int virt_read(uint32_t addr, int size, uint32_t *v)
{
	uint8_t b[4];
	int i;

	for (i = 0; i < size; i++) {
		uint32_t x = addr + i;
		if (x < 8 && !page0)
			b[i] = x < 4 ? 0 : APTR_OF(SysBase) >> (8 * (7 - x));
		else if (x >= 0xdff000 && x < 0xdff200)
			b[i] = custom_read(x - 0xdff000) >> (x & 1 ? 0 : 8);
		else
			return 0;
	}
	*v = size == 1 ? b[0] : size == 2 ? (uint32_t)b[0] << 8 | b[1] :
	     (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
	return 1;
}

static void wr(uint32_t addr, int size, uint32_t v)
{
	if (size == 1)
		*(uint8_t *)(uintptr_t)addr = v;
	else if (size == 2)
		*(uint16_t *)(uintptr_t)addr = v;
	else
		*(uint32_t *)(uintptr_t)addr = v;
}

static int emulate_load(greg_t *g)
{
	const uint16_t *pc = (const uint16_t *)(uintptr_t)g[R_PC];
	uint16_t op = pc[0];
	int smode = (op >> 3) & 7, sreg = op & 7;
	int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7, n, size;
	uint32_t ea, v, *a, mask;

	switch (op >> 12) {                     /* move.b, move.l, move.w */
	case 1: size = 1; break;
	case 2: size = 4; break;
	case 3: size = 2; break;
	default: return 0;
	}
	n = src_ea(g, pc + 1, smode, sreg, &ea);
	if (n < 0 || !virt_read(ea, size, &v))
		return 0;
	if (smode == 3)
		*areg(g, sreg) += size;
	mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	switch (dmode) {
	case 0:
		g[R_D0 + dreg] = (g[R_D0 + dreg] & ~mask) | v;
		break;
	case 1:                                 /* movea sign-extends */
		if (size == 1)
			return 0;
		*areg(g, dreg) = size == 2 ? (uint32_t)(int16_t)v : v;
		break;
	case 2:
		wr(*areg(g, dreg), size, v);
		break;
	case 3:
		a = areg(g, dreg);
		wr(*a, size, v);
		*a += size;
		break;
	case 4:
		a = areg(g, dreg);
		*a -= size;
		wr(*a, size, v);
		break;
	case 5:
		wr(*areg(g, dreg) + (int16_t)pc[1 + n], size, v);
		n++;
		break;
	case 7:
		if (dreg == 0) {                /* abs.w */
			wr((int16_t)pc[1 + n], size, v);
			n++;
		} else if (dreg == 1) {         /* abs.l */
			wr((uint32_t)pc[1 + n] << 16 | pc[2 + n], size, v);
			n += 2;
		} else
			return 0;
		break;
	default:
		return 0;
	}
	if (dmode != 1) {                       /* move sets N and Z */
		g[R_PS] &= ~0x0f;
		if (v & (mask ^ (mask >> 1)))
			g[R_PS] |= 8;
		if (!v)
			g[R_PS] |= 4;
	}
	g[R_PC] += 2 + 2 * n;
	return 1;
}

/*
 * AND, OR and CMP <ea>,Dn and TST <ea> reading address 4 or the custom
 * chips (ShowConfig masks VPOSR with "and.w $dff004,d0").
 */
static int emulate_alu_load(greg_t *g)
{
	const uint16_t *pc = (const uint16_t *)(uintptr_t)g[R_PC];
	uint16_t op = pc[0];
	int smode = (op >> 3) & 7, sreg = op & 7, dreg = (op >> 9) & 7;
	int kind = op >> 12, opmode = (op >> 6) & 7, n, size;
	uint32_t ea, v, d, r, mask, msb;

	if ((op & 0xff00) == 0x4a00 && opmode < 3) {    /* tst */
		kind = 0;
		size = 1 << opmode;
	} else if ((kind == 0x8 || kind == 0xb || kind == 0xc) && opmode < 3)
		size = 1 << opmode;                     /* or, cmp, and */
	else
		return 0;
	n = src_ea(g, pc + 1, smode, sreg, &ea);
	if (n < 0 || !virt_read(ea, size, &v))
		return 0;
	if (smode == 3)
		*areg(g, sreg) += size;
	mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
	msb = mask ^ (mask >> 1);
	d = g[R_D0 + dreg] & mask;
	g[R_PS] &= ~0x0f;                               /* N Z V C; X kept */
	switch (kind) {
	case 0x8: r = d | v; break;
	case 0xc: r = d & v; break;
	case 0xb:
		r = (d - v) & mask;
		if (v > d)
			g[R_PS] |= 1;                   /* C */
		if (((d ^ v) & (d ^ r)) & msb)
			g[R_PS] |= 2;                   /* V */
		break;
	default: r = v; break;
	}
	if (kind == 0x8 || kind == 0xc)
		g[R_D0 + dreg] = (g[R_D0 + dreg] & ~mask) | r;
	if (r & msb)
		g[R_PS] |= 8;
	if (!(r & mask))
		g[R_PS] |= 4;
	g[R_PC] += 2 + 2 * n;
	return 1;
}

/* a MOVE into the custom chips: discarded (CPU writes $DFF004 to see
   whether the bus gives it back) */
/*
 * A store to the custom chips: MOVE (sources Dn, An, #imm, (An), (An)+,
 * d16(An), abs) or CLR to (An), (An)+, -(An), d16(An), abs.  Blitter
 * registers go to blitter.c (which runs the blit); the rest is ignored.
 */
static int emulate_store(greg_t *g)
{
	extern int blitter_write(uint32_t off, int size, uint32_t v);
	const uint16_t *pc = (const uint16_t *)(uintptr_t)g[R_PC];
	uint16_t op = pc[0];
	int smode = (op >> 3) & 7, sreg = op & 7;
	int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7, size, ns = 0, nd, clr = 0;
	uint32_t ea, *a, v = 0, sea = 0;

	switch (op >> 12) {
	case 1: size = 1; break;
	case 2: size = 4; break;
	case 3: size = 2; break;
	case 4:
		if ((op & 0xff00) != 0x4200 || ((op >> 6) & 3) == 3)
			return 0;
		size = 1 << ((op >> 6) & 3);
		dmode = smode;                  /* CLR's effective address */
		dreg = sreg;
		clr = 1;
		break;
	default: return 0;
	}
	if (clr)
		v = 0;
	else if (smode == 0)
		v = g[R_D0 + sreg];
	else if (smode == 1)
		v = *areg(g, sreg);
	else if (smode == 7 && sreg == 4) {     /* #imm */
		ns = size == 4 ? 2 : 1;
		v = size == 4 ? (uint32_t)pc[1] << 16 | pc[2] : pc[1];
	} else {
		/* read once the destination is known to be ours: the fault
		   may be the source's */
		ns = src_ea(g, pc + 1, smode, sreg, &sea);
		if (ns < 0)
			return 0;
	}
	a = areg(g, dreg);
	switch (dmode) {
	case 2: case 3: ea = *a; nd = 0; break;
	case 4: ea = *a - size; nd = 0; break;
	case 5: ea = *a + (int16_t)pc[1 + ns]; nd = 1; break;
	case 7:
		if (dreg == 0) {
			ea = (int16_t)pc[1 + ns];
			nd = 1;
		} else if (dreg == 1) {
			ea = (uint32_t)pc[1 + ns] << 16 | pc[2 + ns];
			nd = 2;
		} else
			return 0;
		break;
	default:
		return 0;
	}
	if (ea < 0xdff000 || ea >= 0xdff200)
		return 0;
	if (!clr && smode >= 2 && !(smode == 7 && sreg == 4))
		v = size == 1 ? *(uint8_t *)(uintptr_t)sea :
		    size == 2 ? *(uint16_t *)(uintptr_t)sea : *(uint32_t *)(uintptr_t)sea;
	if (!clr && smode == 3)
		*areg(g, sreg) += size == 1 && sreg == 7 ? 2 : size;
	if (dmode == 3)
		*a += size;
	else if (dmode == 4)
		*a -= size;
	if (size == 1)
		v &= 0xff;
	else if (size == 2)
		v &= 0xffff;
	if (!blitter_write(ea - 0xdff000, size, v) && af_trace)
		af_warn("write to custom chip register $%x ignored", ea);
	g[R_PC] += 2 + 2 * (ns + nd);
	return 1;
}

/*
 * Privileged instructions.  MOVE from SR is privileged on the 68010 and
 * up but plain user code on a 68000, so it is emulated always; the rest
 * only inside Supervisor().
 */
static uint32_t movec_read(int rc, greg_t *g)
{
	int cpu = SysBase && (SysBase->AttnFlags & AFF_68060) ? 68060 : 68040;

	switch (rc) {
	case 0x002:                             /* CACR: both caches on */
		return cpu == 68060 ? 0xa0808000 : 0x80008000;
	case 0x003:                             /* TC: MMU on, 4K pages */
		return 0x8000;
	case 0x800:                             /* USP */
		return g[R_SP];
	case 0x808:                             /* PCR: 68060, revision 6 */
		return 0x04300600;
	default:                                /* SFC DFC TTx VBR ... */
		return 0;
	}
}

static uint32_t in_trap;        /* the stack pointer a task trap's RTE leaves */

static int emulate_priv(greg_t *g)
{
	uint16_t *pc = (uint16_t *)(uintptr_t)g[R_PC];
	uint16_t op = pc[0];
	uint32_t *sp, *rp;
	int reg = op & 7;

	if ((op & 0xfff8) == 0x40c0) {          /* move sr,dn */
		g[R_D0 + reg] = (g[R_D0 + reg] & 0xffff0000) |
				(g[R_PS] & 0xff) | (in_supervisor ? 0x2000 : 0);
		g[R_PC] += 2;
		return 1;
	}
	if (!in_supervisor)
		return 0;
	switch (op) {
	case 0x4e73: {                          /* rte */
		uint8_t *s = (uint8_t *)(uintptr_t)g[R_SP];
		int fmt = (s[6] >> 4) & 15;
		g[R_PS] = (g[R_PS] & ~0xff) | s[1];
		g[R_PC] = (uint32_t)s[2] << 24 | s[3] << 16 | s[4] << 8 | s[5];
		g[R_SP] += fmt == 2 || fmt == 3 ? 12 : fmt == 4 ? 16 : 8;
		if (in_trap && g[R_SP] == in_trap) {   /* the end of a trap handler */
			in_trap = 0;
			in_supervisor--;
		}
		return 1;
	}
	case 0x4e7a:                            /* movec rc,rn */
		rp = (uint32_t *)&g[(pc[1] & 0x8000 ? R_A0 : R_D0) +
				    ((pc[1] >> 12) & 7)];
		*rp = movec_read(pc[1] & 0xfff, g);
		g[R_PC] += 4;
		return 1;
	case 0x4e7b:                            /* movec rn,rc: accepted */
	case 0x007c:                            /* ori.w #x,sr */
	case 0x027c:                            /* andi.w #x,sr */
	case 0x0a7c:                            /* eori.w #x,sr */
	case 0x46fc:                            /* move #x,sr */
		g[R_PC] += 4;
		return 1;
	}
	if ((op & 0xfff8) == 0x46c0 ||          /* move dn,sr */
	    (op & 0xfff8) == 0x4e60) {          /* move an,usp */
		g[R_PC] += 2;
		return 1;
	}
	if ((op & 0xfff8) == 0x4e68) {          /* move usp,an */
		g[R_A0 + reg] = g[R_SP];
		g[R_PC] += 2;
		return 1;
	}
	if ((op & 0xff00) == 0xf400 || (op & 0xff00) == 0xf500) {
		cache_flush(0, 0xffffffff);     /* cinv/cpush, pflush */
		g[R_PC] += 2;
		return 1;
	}
	if ((op & 0xff80) == 0xf300) {          /* fsave/frestore: null frames */
		int mode = (op >> 3) & 7, save = !(op & 0x40);
		rp = (uint32_t *)&g[R_A0 + reg];
		if (mode == 4 && save) {
			*rp -= 4;
			sp = (uint32_t *)(uintptr_t)*rp;
			*sp = 0;
		} else if (mode == 3 && !save) {
			*rp += 4;
		} else if (mode == 2) {
			if (save)
				*(uint32_t *)(uintptr_t)*rp = 0;
		} else
			return 0;
		g[R_PC] += 2;
		return 1;
	}
	return 0;
}

/*
 * A task with its own tc_TrapCode gets the processor exceptions, as on
 * AmigaOS: the handler runs "in supervisor mode" on an exception frame
 * (format 0) with the trap number pushed above it, and its RTE returns
 * to the program.  ShowConfig finds out whether there is an MMU this way.
 */
static int task_trap(int sig, siginfo_t *si, greg_t *g)
{
	struct Task *t = SysBase ? PTR(SysBase->ThisTask) : NULL;
	uint16_t op = *(uint16_t *)(uintptr_t)g[R_PC];
	uint32_t vec, *sp;
	uint16_t *w;

	if (!t || !t->tc_TrapCode || t->tc_TrapCode == SysBase->TaskTrapCode ||
	    in_trap)
		return 0;
	if (sig == SIGILL)
		vec = si->si_code == ILL_PRVOPC ? 8 : (op >> 12) == 0xf ? 11 :
		      (op >> 12) == 0xa ? 10 : 4;
	else if (sig == SIGFPE && si->si_code == FPE_INTDIV)
		vec = 5;
	else
		return 0;
	sp = (uint32_t *)(uintptr_t)(g[R_SP] - 12);
	w = (uint16_t *)(sp + 1);
	w[0] = (g[R_PS] & 0xff) | 0x2000;       /* SR */
	w[1] = g[R_PC] >> 16;                   /* PC */
	w[2] = g[R_PC];
	w[3] = vec * 4;                         /* format 0, vector offset */
	sp[0] = vec;                            /* the trap number */
	in_trap = g[R_SP];
	in_supervisor++;
	g[R_SP] = (uintptr_t)sp;
	g[R_PC] = t->tc_TrapCode;
	return 1;
}

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv;
	greg_t *g = uc->uc_mcontext.gregs;
	uint32_t pc = g[R_PC], off;
	int h, i;

	if (sig == SIGSEGV && (uint32_t)(uintptr_t)si->si_addr == pc)
		goto crash;     /* a jump into nowhere: no instruction to decode */
	if (sig == SIGSEGV && screen_plane_fault((uint32_t)(uintptr_t)si->si_addr))
		return;         /* a write to a screen's planes: noted, page open */
	if (sig == SIGSEGV && SysBase && (emulate_load(g) || emulate_store(g) ||
					  emulate_alu_load(g)))
		return;
	if ((sig == SIGILL || sig == SIGSEGV) && emulate_priv(g))
		return;
	if (task_trap(sig, si, g))
		return;
crash:
	dos_flush_all();
	fprintf(stderr, "amigafmt: %s: %s at $%08x, address $%08lx", af_progname,
		strsignal(sig), pc, (unsigned long)(uintptr_t)si->si_addr);
	if (hunk_where(pc, the_seg, &h, &off))
		fprintf(stderr, " (hunk %d + $%x)", h, off);
	fputc('\n', stderr);
	for (i = 0; i < 8; i++)
		fprintf(stderr, "  d%d=%08x a%d=%08x\n", i, (uint32_t)g[R_D0 + i],
			i, (uint32_t)g[R_A0 + i]);
	if ((uint32_t)g[R_SP] < stack_lo + 64 && (uint32_t)g[R_SP] >= stack_lo - 4096)
		fprintf(stderr, "amigafmt: the stack is exhausted; try -s "
			"with more than %u bytes\n", stack_hi - stack_lo);
	_exit(20);
}

/* AMIGAFMT_HANGDUMP=secs: say where the program is every so often (a hang) */
static void on_alarm(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv;
	greg_t *g = uc->uc_mcontext.gregs;
	char buf[160];
	int n, h;
	uint32_t off;

	(void)sig;
	(void)si;
	extern unsigned long screen_plane_faults;

	n = snprintf(buf, sizeof(buf), "amigafmt: hangdump: pc $%08x a6 $%08x in call %x, "
		     "%lu plane faults", (uint32_t)g[R_PC], (uint32_t)g[R_A0 + 6],
		     resume_frame ? resume_frame->id : 0, screen_plane_faults);
	if (hunk_where(g[R_PC], the_seg, &h, &off))
		n += snprintf(buf + n, sizeof(buf) - n, " (hunk %d + $%x)", h, off);
	buf[n++] = '\n';
	if (write(2, buf, n) < 0)
		return;
	n = snprintf(buf, sizeof(buf), "  d %x %x %x %x %x %x %x %x\n", (uint32_t)g[R_D0],
		     (uint32_t)g[R_D0 + 1], (uint32_t)g[R_D0 + 2], (uint32_t)g[R_D0 + 3],
		     (uint32_t)g[R_D0 + 4], (uint32_t)g[R_D0 + 5], (uint32_t)g[R_D0 + 6],
		     (uint32_t)g[R_D0 + 7]);
	if (write(2, buf, n) < 0)
		return;
	n = snprintf(buf, sizeof(buf), "  a %x %x %x %x %x %x %x %x\n", (uint32_t)g[R_A0],
		     (uint32_t)g[R_A0 + 1], (uint32_t)g[R_A0 + 2], (uint32_t)g[R_A0 + 3],
		     (uint32_t)g[R_A0 + 4], (uint32_t)g[R_A0 + 5], (uint32_t)g[R_A0 + 6],
		     (uint32_t)g[R_SP]);
	if (write(2, buf, n) < 0)
		return;
}

static void map_page0(int allow)
{
	void *p;

	if (!allow)
		return;
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p != (void *)0)
		return;
	__asm__("" : "+r"(p));          /* gcc must not reason about NULL */
	((volatile uint32_t *)p)[1] = APTR_OF(SysBase);
	mprotect(p, 4096, PROT_READ);
	page0 = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: amigafmt [options] program [arguments...]\n"
		"       amigafmt [options] -c command-line [-c command-line...]\n"
		"  -a NAME=PATH  assign NAME: to a Linux directory or Amiga path\n"
		"  -c LINE       run LINE through the AmigaOS shell (repeatable)\n"
		"  -s BYTES      stack size (default 262144)\n"
		"  -t            trace every library call to stderr\n"
		"  -u            a leading / is the Linux root, not the parent\n"
		"  -n            do not map page 0 (emulate reads of address 4)\n"
		"  -l            list the assigns and exit\n");
	exit(20);
}

int main(int argc, char **argv)
{
	uint32_t stack = 262144, *sp;
	int c, nopage0 = getenv("AMIGAFMT_NOPAGE0") != NULL, list = 0, err;
	struct sigaction sa;
	struct regs r;
	char *ap[64];
	int nap = 0;
	const char *prog, *b;
	char *lp;
	uint8_t *st;
	uint32_t rc;
	stack_t ss;
	char *shell = NULL;

	af_trace = getenv("AMIGAFMT_TRACE") != NULL;
	while ((c = getopt(argc, argv, "+a:c:s:tunlh")) != -1) {
		switch (c) {
		case 'c': {
			size_t n = shell ? strlen(shell) : 0;
			shell = realloc(shell, n + strlen(optarg) + 2);
			sprintf(shell + n, "%s\n", optarg);
			break;
		}
		case 'a':
			if (nap < 64)
				ap[nap++] = optarg;
			break;
		case 's':
			stack = strtoul(optarg, NULL, 0);
			break;
		case 't':
			af_trace = 1;
			break;
		case 'u':
			setenv("AMIGAFMT_UNIXPATHS", "1", 1);
			break;
		case 'n':
			nopage0 = 1;
			break;
		case 'l':
			list = 1;
			break;
		default:
			usage();
		}
	}
	if (optind >= argc && !list && !shell)
		usage();
	prog = list ? NULL : shell ? "Shell" : argv[optind];
	path_init(prog);
	for (c = 0; c < nap; c++) {
		char *eq = strchr(ap[c], '=');
		if (!eq)
			usage();
		*eq = 0;
		path_assign(ap[c], eq + 1);
	}
	if (list) {
		path_dump(stdout);
		return 0;
	}
	b = strrchr(prog, '/');
	af_progname = b ? b + 1 : prog;
	if (stack < 8192)
		stack = 8192;
	stack = (stack + 3) & ~3u;
	if (getenv("WBDE_STACK") && strtoul(getenv("WBDE_STACK"), NULL, 0) > stack)
		stack = (strtoul(getenv("WBDE_STACK"), NULL, 0) + 3) & ~3u;

	/* a Linux path is what the shell gave us; fall back to Amiga names */
	lp = shell ? NULL : access(prog, R_OK) ? path_to_linux(prog, NULL) :
	     strdup(prog);
	if (!shell && (!lp || !(the_seg = hunk_load(lp, &err)))) {
		fprintf(stderr, "amigafmt: %s: cannot load (%s)\n", prog,
			lp && !access(lp, R_OK) ? "not an AmigaOS executable" :
			strerror(errno));
		return 20;
	}
	free(lp);

	exec_init();
	map_page0(!nopage0);
	if (af_trace)
		fprintf(stderr, "amigafmt: ExecBase at $%08x, address 4 %s\n",
			APTR_OF(SysBase), page0 ? "mapped (page 0)" :
			"emulated (SIGSEGV)");
	dos_init(shell ? 0 : argc - optind - 1, argv + optind + 1, prog,
		 the_seg, stack);
	shell_init();
	{
		extern void task_init(void);
		task_init();
	}

	memset(&ss, 0, sizeof(ss));
	ss.ss_size = 65536;
	ss.ss_sp = malloc(ss.ss_size);
	sigaltstack(&ss, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	if (getenv("AMIGAFMT_HANGDUMP")) {
		struct sigaction al;
		struct itimerval it = { { atoi(getenv("AMIGAFMT_HANGDUMP")), 0 },
					{ atoi(getenv("AMIGAFMT_HANGDUMP")), 0 } };
		memset(&al, 0, sizeof(al));
		al.sa_sigaction = on_alarm;
		al.sa_flags = SA_SIGINFO | SA_RESTART;
		sigaction(SIGALRM, &al, NULL);
		setitimer(ITIMER_REAL, &it, NULL);
	}

	if (shell) {
		rc = shell_main(shell);
		dos_flush_all();
		return (int32_t)rc < 0 || rc > 255 ? 255 : (int)rc;
	}
	st = amem_alloc(stack + 64);
	stack_lo = APTR_OF(st);
	stack_hi = stack_lo + stack;
	me->pr_Task.tc_SPLower = stack_lo;
	me->pr_Task.tc_SPUpper = stack_hi;
	me->pr_StackBase = MKBADDR(stack_hi);
	sp = (uint32_t *)(st + stack);
	*--sp = stack;                          /* 4(sp) = stack size */
	memset(&r, 0, sizeof(r));
	r.a[7] = APTR_OF(sp);
	r.d[0] = dos_arglen;
	r.a[0] = APTR_OF(dos_args);
	r.a[6] = APTR_OF(SysBase);
	rc = call_amiga(&r, APTR_OF((uint32_t *)BADDR(the_seg) + 1));
	rc = shell_after(rc);           /* a script it started (C:Execute) */
	dos_flush_all();
	if (af_trace)
		fprintf(stderr, "amigafmt: %s returned %d\n", af_progname,
			(int)rc);
	return (int32_t)rc < 0 || rc > 255 ? 255 : (int)rc;
}
