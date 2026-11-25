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
#include <ucontext.h>
#include <unistd.h>
#include "af.h"

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
	case 0x002: return 0x03f0;              /* DMACONR */
	case 0x004: return 0x3300;              /* VPOSR: Alice, NTSC */
	case 0x07c: return 0x00f8;              /* DENISEID: Lisa */
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

/* a MOVE into the custom chips: discarded (CPU writes $DFF004 to see
   whether the bus gives it back) */
static int emulate_store(greg_t *g)
{
	const uint16_t *pc = (const uint16_t *)(uintptr_t)g[R_PC];
	uint16_t op = pc[0];
	int smode = (op >> 3) & 7, sreg = op & 7;
	int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7, size, ns = 0, nd;
	uint32_t ea, *a;

	switch (op >> 12) {
	case 1: size = 1; break;
	case 2: size = 4; break;
	case 3: size = 2; break;
	default: return 0;
	}
	if (smode == 7 && sreg == 4)            /* #imm */
		ns = size == 4 ? 2 : 1;
	else if (smode > 1)
		return 0;                       /* only Dn, An, #imm sources */
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
	if (dmode == 3)
		*a += size;
	else if (dmode == 4)
		*a -= size;
	if (af_trace)
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

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv;
	greg_t *g = uc->uc_mcontext.gregs;
	uint32_t pc = g[R_PC], off;
	int h, i;

	if (sig == SIGSEGV && SysBase && (emulate_load(g) || emulate_store(g)))
		return;
	if ((sig == SIGILL || sig == SIGSEGV) && emulate_priv(g))
		return;
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
		"  -a NAME=PATH  assign NAME: to a Linux directory or Amiga path\n"
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

	af_trace = getenv("AMIGAFMT_TRACE") != NULL;
	while ((c = getopt(argc, argv, "+a:s:tunlh")) != -1) {
		switch (c) {
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
	if (optind >= argc && !list)
		usage();
	prog = list ? NULL : argv[optind];
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

	/* a Linux path is what the shell gave us; fall back to Amiga names */
	lp = access(prog, R_OK) ? path_to_linux(prog, NULL) : strdup(prog);
	if (!lp || !(the_seg = hunk_load(lp, &err))) {
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
	dos_init(argc - optind - 1, argv + optind + 1, prog, the_seg, stack);

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
	dos_flush_all();
	if (af_trace)
		fprintf(stderr, "amigafmt: %s returned %d\n", af_progname,
			(int)rc);
	return (int32_t)rc < 0 || rc > 255 ? 255 : (int)rc;
}
