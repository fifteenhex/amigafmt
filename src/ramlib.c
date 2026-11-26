/*
 * Libraries from disk, as ramlib loads them: OpenLibrary of a name that
 * is not one of amigafmt's own LoadSegs LIBS:name (or the path it was
 * given, "PROGDIR:libs/x.library"), finds the RomTag in the segments,
 * builds the library the way InitResident does for RTF_AUTOINIT (an
 * init table: data size, vectors, InitStruct table, init routine) and
 * puts it on SysBase->LibList; then the library's own Open vector runs.
 * The library's code is the program's kind of code: it runs natively and
 * calls amigafmt's exec through a6 like anything else.
 *
 * Also exec's MakeLibrary, MakeFunctions, InitResident, AddLibrary,
 * RemLibrary, which programs use to build libraries of their own.
 *
 * amigafmt's own libraries always win: a Commodore disk library such as
 * LIBS:asl.library is never loaded over the built-in one, because it
 * would call into the system's private parts.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "af.h"
#include "dos_int.h"

#define RTC_MATCHWORD 0x4afc
#define RTF_AUTOINIT  0x80

extern void exec_initstruct(const uint8_t *t, uint8_t *base, uint32_t size);
extern void exec_addtail(void *l, void *n);

static uint16_t rd16(const void *p)
{
	const uint8_t *b = p;
	return b[0] << 8 | b[1];
}

static uint32_t rd32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* MakeFunctions(target, functionArray, funcDispBase): vectors below target */
static uint32_t make_functions(uint8_t *target, const uint8_t *fa, uint32_t disp)
{
	int rel = rd16(fa) == 0xffff, n = 0;
	const uint8_t *p = rel ? fa + 2 : fa;

	if (rel && disp == 0)
		disp = (uint32_t)(uintptr_t)fa;
	for (;; n++) {
		uint32_t fn;
		uint8_t *v;
		if (rel) {
			int16_t o = (int16_t)rd16(p);
			if (o == -1)
				break;
			fn = disp + o;
			p += 2;
		} else {
			fn = rd32(p);
			if (fn == 0xffffffff)
				break;
			if (disp)
				fn += disp;
			p += 4;
		}
		v = target - (n + 1) * 6;
		v[0] = 0x4e, v[1] = 0xf9;
		v[2] = fn >> 24, v[3] = fn >> 16, v[4] = fn >> 8, v[5] = fn;
	}
	cache_flush(target - n * 6, n * 6);
	return n * 6;
}

static int count_functions(const uint8_t *fa)
{
	int rel = rd16(fa) == 0xffff, n = 0;
	const uint8_t *p = rel ? fa + 2 : fa;

	if (rel)
		while ((int16_t)rd16(p + 2 * n) != -1)
			n++;
	else
		while (rd32(p + 4 * n) != 0xffffffff)
			n++;
	return n;
}

/* MakeLibrary: d0 = the library, or 0 */
static uint32_t make_library(struct frame *f, uint32_t vectors, uint32_t structure,
			     uint32_t init, uint32_t dsize, uint32_t seglist,
			     const uint8_t *rt)
{
	int n = count_functions(PTR(vectors));
	uint32_t neg = (n * 6 + 3) & ~3u;
	uint8_t *mem;
	struct Library *lib;

	if (dsize < sizeof(struct Library))
		dsize = sizeof(struct Library);
	mem = amem_alloc(neg + dsize);
	if (!mem)
		return 0;
	lib = (struct Library *)(mem + neg);
	make_functions((uint8_t *)lib, PTR(vectors), 0);
	if (structure)
		exec_initstruct(PTR(structure), (uint8_t *)lib, 0);
	if (rt) {
		/* InitResident fills in what the RomTag says, for libraries
		   whose init table has no InitStruct table
		   (LIBS:commodities.library) */
		if (!lib->lib_Node.ln_Type)
			lib->lib_Node.ln_Type = rt[12];
		if (!lib->lib_Node.ln_Name)
			lib->lib_Node.ln_Name = rd32(rt + 14);
		if (!lib->lib_Version)
			lib->lib_Version = rt[11];
		if (!lib->lib_IdString)
			lib->lib_IdString = rd32(rt + 18);
		lib->lib_Flags |= 6;            /* LIBF_SUMUSED | LIBF_CHANGED */
	}
	lib->lib_NegSize = neg;
	lib->lib_PosSize = dsize;
	if (init) {
		struct regs r;
		memset(&r, 0, sizeof(r));
		r.d[0] = APTR_OF(lib);
		r.a[0] = seglist;
		r.a[6] = APTR_OF(SysBase);
		return amiga_call(f, init, &r);
	}
	return APTR_OF(lib);
}

static void add_library(struct Library *lib)
{
	lib->lib_Flags |= 2;                    /* LIBF_CHANGED */
	exec_addtail(&SysBase->LibList, &lib->lib_Node);
}

/* InitResident(resident, segList) */
static uint32_t init_resident(struct frame *f, uint8_t *rt, uint32_t seglist)
{
	uint32_t init = rd32(rt + 22);          /* rt_Init */
	uint32_t res;

	if (rd16(rt) != RTC_MATCHWORD)
		return 0;
	if (rt[10] & RTF_AUTOINIT) {
		const uint8_t *it = PTR(init);
		res = make_library(f, rd32(it + 4), rd32(it + 8), rd32(it + 12),
				   rd32(it), seglist, rt);
		if (res && rt[12] == NT_LIBRARY)
			add_library(PTR(res));
		return res;
	}
	{
		struct regs r;
		memset(&r, 0, sizeof(r));
		r.a[0] = seglist;
		r.a[6] = APTR_OF(SysBase);
		return amiga_call(f, init, &r);
	}
}

/* the RomTag in a seglist: rt_MatchWord with rt_MatchTag pointing at itself */
static uint8_t *find_romtag(BPTR seg)
{
	for (; seg; seg = *(uint32_t *)BADDR(seg)) {
		uint32_t *h = BADDR(seg);
		uint8_t *p = (uint8_t *)(h + 1);
		uint32_t len = h[-1] - 8, i;
		for (i = 0; i + 26 <= len; i += 2)
			if (rd16(p + i) == RTC_MATCHWORD &&
			    rd32(p + i + 2) == (uint32_t)(uintptr_t)(p + i))
				return p + i;
	}
	return NULL;
}

static const char *base_name(const char *n)
{
	const char *p = strrchr(n, '/'), *q = strrchr(n, ':');

	if (q && (!p || q > p))
		p = q;
	return p ? p + 1 : n;
}

/* a library some disk loaded before, by name */
static struct Library *in_list(const char *name)
{
	struct Node *n;

	for (n = PTR(SysBase->LibList.lh_Head); n && n->ln_Succ; n = PTR(n->ln_Succ))
		if (n->ln_Name && !strcmp(PTR(n->ln_Name), name))
			return (struct Library *)n;
	return NULL;
}

/* call a library's Open vector (-6): its d0 is the result */
static uint32_t lib_open_vector(struct frame *f, struct Library *lib, uint32_t version)
{
	struct regs r;

	memset(&r, 0, sizeof(r));
	r.d[0] = version;
	r.a[6] = APTR_OF(lib);
	return amiga_call(f, APTR_OF(lib) - 6, &r);
}

/* is this library one of amigafmt's own (C vectors)? */
static int ours(struct Library *lib)
{
	int i;

	for (i = 0; all_libs[i]; i++)
		if (all_libs[i]->base == lib)
			return 1;
	return 0;
}

uint32_t ramlib_open(struct frame *f, const char *name, uint32_t version)
{
	const char *bn = base_name(name);
	struct Library *lib = in_list(bn);
	char apath[512];
	char *lp;
	BPTR seg = 0;
	int err, i;
	uint8_t *rt;
	uint32_t res;

	if (lib && !ours(lib)) {
		if (version > lib->lib_Version)
			return 0;
		return lib_open_vector(f, lib, version);
	}
	for (i = 0; i < 3 && !seg; i++) {
		if (i == 0) {
			if (!strchr(name, ':') && !strchr(name, '/'))
				continue;
			snprintf(apath, sizeof(apath), "%s", name);
		} else if (i == 1)
			snprintf(apath, sizeof(apath), "LIBS:%s", bn);
		else
			snprintf(apath, sizeof(apath), "PROGDIR:Libs/%s", bn);
		lp = path_to_linux(apath, NULL);
		if (lp)
			seg = hunk_load(lp, &err);
		free(lp);
	}
	if (!seg)
		return 0;
	rt = find_romtag(seg);
	if (!rt || rt[12] != NT_LIBRARY) {
		af_warn("%s: %s has no library RomTag", af_progname, apath);
		hunk_unload(seg);
		return 0;
	}
	res = init_resident(f, rt, seg);
	if (!res) {
		af_warn("%s: %s did not initialise", af_progname, apath);
		return 0;
	}
	lib = PTR(res);
	if (af_trace)
		af_warn("loaded %s from %s: version %d.%d, %d vectors", bn, apath,
			lib->lib_Version, lib->lib_Revision, lib->lib_NegSize / 6);
	if (!in_list(bn))
		add_library(lib);
	if (version > lib->lib_Version)
		return 0;
	return lib_open_vector(f, lib, version);
}

/* CloseLibrary of a disk library: its Close vector (-12); an expunge's
   seglist is not unloaded (nothing frees code under a running program) */
int ramlib_close(struct frame *f, struct Library *lib)
{
	struct regs r;

	if (!lib || ours(lib))
		return 0;
	memset(&r, 0, sizeof(r));
	r.a[6] = APTR_OF(lib);
	amiga_call(f, APTR_OF(lib) - 12, &r);
	return 1;
}

/* ---- exec vectors ---- */

void e_MakeLibrary(struct frame *f)
{
	RET(make_library(f, A(0), A(1), A(2), D(0), D(1), NULL));
}

void e_MakeFunctions(struct frame *f)
{
	RET(make_functions(PTR(A(0)), PTR(A(1)), A(2)));
}

void e_InitResident(struct frame *f)
{
	RET(init_resident(f, PTR(A(1)), D(1)));
}

void e_AddLibrary(struct frame *f)
{
	add_library(PTR(A(1)));
}

void e_RemLibrary(struct frame *f)
{
	struct Library *lib = PTR(A(1));
	struct regs r;

	if (!lib || ours(lib)) {
		RET(0);
		return;
	}
	memset(&r, 0, sizeof(r));
	r.a[6] = APTR_OF(lib);
	RET(amiga_call(f, APTR_OF(lib) - 18, &r));      /* Expunge */
}
