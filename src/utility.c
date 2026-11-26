/*
 * utility.library: tags, hooks, date conversion, 32/64-bit arithmetic
 * (SAS/C-compiled programs such as the 3.1 C: commands call UMult32 and
 * friends for every multiply and divide), and Latin-1 case handling.
 * Plus version.library, which only exists to carry a version number.
 */
#include <string.h>
#include <strings.h>
#include "af.h"

struct Library *UtilityBase;

static int up(int c)
{
	c &= 0xff;
	if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
		return c - 32;
	return c;
}

static int lo(int c)
{
	c &= 0xff;
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
		return c + 32;
	return c;
}

static void u_ToUpper(struct frame *f) { RET(up(D(0))); }
static void u_ToLower(struct frame *f) { RET(lo(D(0))); }

static int stricmp_n(const uint8_t *a, const uint8_t *b, int32_t n)
{
	for (; n; n--, a++, b++) {
		int x = up(*a), y = up(*b);
		if (x != y)
			return x < y ? -1 : 1;
		if (!x)
			return 0;
	}
	return 0;
}

static void u_Stricmp(struct frame *f)
{
	RET(stricmp_n(PTR(A(0)), PTR(A(1)), -1));
}

static void u_Strnicmp(struct frame *f)
{
	RET((int32_t)D(0) <= 0 ? 0 : stricmp_n(PTR(A(0)), PTR(A(1)), D(0)));
}

static void u_SMult32(struct frame *f) { RET((int32_t)D(0) * (int32_t)D(1)); }
static void u_UMult32(struct frame *f) { RET(D(0) * D(1)); }

/*
 * 64-bit results: low half in d0, high half in d1, which is what the
 * ROM's "mulu.l d1,d1:d0" leaves; the autodoc's "D0:D1" reads the other
 * way, but C:Info relies on this order.
 */
static void u_UMult64(struct frame *f)
{
	uint64_t r = (uint64_t)D(0) * D(1);
	D(1) = r >> 32;
	RET((uint32_t)r);
}

static void u_SMult64(struct frame *f)
{
	int64_t r = (int64_t)(int32_t)D(0) * (int32_t)D(1);
	D(1) = (uint64_t)r >> 32;
	RET((uint32_t)r);
}

static void u_UDivMod32(struct frame *f)
{
	uint32_t a = D(0), b = D(1);

	if (!b) {
		D(1) = 0;
		RET(0);
		return;
	}
	D(1) = a % b;
	RET(a / b);
}

static void u_SDivMod32(struct frame *f)
{
	int32_t a = D(0), b = D(1);

	if (!b || (a == (int32_t)0x80000000 && b == -1)) {
		D(1) = 0;
		RET(0);
		return;
	}
	D(1) = a % b;
	RET(a / b);
}

/* ---- dates: seconds since 1978-01-01 ---- */

static void u_Amiga2Date(struct frame *f)
{
	struct ClockData *cd = PTR(A(0));
	uint32_t s = D(0), days = s / 86400, y = 1978;
	static const uint8_t ml[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31,
				      30, 31 };
	int m;

	cd->sec = s % 60;
	cd->min = s / 60 % 60;
	cd->hour = s / 3600 % 24;
	cd->wday = days % 7;            /* 1978-01-01 was a Sunday */
	for (;;) {
		uint32_t yl = (y % 4 == 0 && (y % 100 || y % 400 == 0)) ? 366 : 365;
		if (days < yl)
			break;
		days -= yl;
		y++;
	}
	for (m = 0; m < 12; m++) {
		uint32_t l = ml[m] + (m == 1 && y % 4 == 0 &&
				      (y % 100 || y % 400 == 0));
		if (days < l)
			break;
		days -= l;
	}
	cd->year = y;
	cd->month = m + 1;
	cd->mday = days + 1;
}

static uint32_t date2amiga(const struct ClockData *cd)
{
	static const uint16_t cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243,
					273, 304, 334 };
	uint32_t d = 0, y;

	for (y = 1978; y < cd->year; y++)
		d += (y % 4 == 0 && (y % 100 || y % 400 == 0)) ? 366 : 365;
	d += cum[(cd->month - 1) % 12] + cd->mday - 1;
	if (cd->month > 2 && cd->year % 4 == 0 &&
	    (cd->year % 100 || cd->year % 400 == 0))
		d++;
	return d * 86400 + cd->hour * 3600 + cd->min * 60 + cd->sec;
}

static void u_Date2Amiga(struct frame *f) { RET(date2amiga(PTR(A(0)))); }

static void u_CheckDate(struct frame *f)
{
	struct ClockData *cd = PTR(A(0)), t;

	if (cd->month < 1 || cd->month > 12 || cd->mday < 1 || cd->hour > 23 ||
	    cd->min > 59 || cd->sec > 59 || cd->year < 1978) {
		RET(0);
		return;
	}
	struct frame g = *f;
	g.d[0] = date2amiga(cd);
	g.a[0] = APTR_OF(&t);
	u_Amiga2Date(&g);
	RET(t.mday == cd->mday ? date2amiga(cd) : 0);
}

/* ---- tags ---- */

#define TAG_DONE   0
#define TAG_IGNORE 1
#define TAG_MORE   2
#define TAG_SKIP   3

struct TagItem *util_next_tag(struct TagItem **tp);
static struct TagItem *next_tag(struct TagItem **tp)
{
	return util_next_tag(tp);
}

struct TagItem *util_next_tag(struct TagItem **tp)
{
	struct TagItem *t = *tp;

	while (t) {
		switch (t->ti_Tag) {
		case TAG_DONE:
			*tp = NULL;
			return NULL;
		case TAG_IGNORE:
			t++;
			break;
		case TAG_MORE:
			t = PTR(t->ti_Data);
			break;
		case TAG_SKIP:
			t += t->ti_Data + 1;
			break;
		default:
			*tp = t + 1;
			return t;
		}
	}
	*tp = NULL;
	return NULL;
}

static void u_NextTagItem(struct frame *f)
{
	struct TagItem **tp = PTR(A(0));
	struct TagItem *t = PTR(*(uint32_t *)tp);
	struct TagItem *r = next_tag(&t);

	*(uint32_t *)tp = APTR_OF(t);
	RET(r);
}

static struct TagItem *find_tag(uint32_t tag, struct TagItem *t)
{
	struct TagItem *x;

	while ((x = next_tag(&t)))
		if (x->ti_Tag == tag)
			return x;
	return NULL;
}

static void u_FindTagItem(struct frame *f) { RET(find_tag(D(0), PTR(A(0)))); }

static void u_GetTagData(struct frame *f)
{
	struct TagItem *x = find_tag(D(0), PTR(A(0)));
	RET(x ? x->ti_Data : D(1));
}

static void u_TagInArray(struct frame *f)
{
	uint32_t *a = PTR(A(0));

	for (; *a != TAG_DONE; a++)
		if (*a == D(0)) {
			RET(1);
			return;
		}
	RET(0);
}

static void u_PackBoolTags(struct frame *f)
{
	struct TagItem *t = PTR(A(0)), *x;
	uint32_t *map = PTR(A(1)), flags = D(0);

	while ((x = next_tag(&t))) {
		uint32_t *m;
		for (m = map; m[0]; m += 2)
			if (m[0] == x->ti_Tag) {
				if (x->ti_Data)
					flags |= m[1];
				else
					flags &= ~m[1];
			}
	}
	RET(flags);
}

static void u_AllocateTagItems(struct frame *f)
{
	RET(amem_alloc((D(0) + 1) * sizeof(struct TagItem)));
}

static void u_FreeTagItems(struct frame *f) { amem_free(PTR(A(0))); }

static void u_CloneTagItems(struct frame *f)
{
	struct TagItem *t = PTR(A(0)), *x, *r, *o;
	int n = 0;

	for (struct TagItem *c = t; next_tag(&c); )
		n++;
	r = o = amem_alloc((n + 1) * sizeof(*r));
	while ((x = next_tag(&t)))
		*o++ = *x;
	RET(r);
}

static void u_CallHookPkt(struct frame *f)
{
	struct Hook *h = PTR(A(0));
	struct regs r;

	regs_caller(&r, f);
	r.a[0] = A(0);
	r.a[1] = A(1);
	r.a[2] = A(2);
	RET(amiga_call(f, h->h_Entry, &r));
}

static void u_GetUniqueID(struct frame *f)
{
	static uint32_t id;
	RET(++id);
}

/*
 * Named objects and name spaces (utility/name.h).  The program sees only
 * no_Object, the first long; the rest is ours.  The root name space is
 * a static object.
 */
#define ANO_NameSpace 4000
#define ANO_UserSpace 4001
#define ANO_Priority  4002
#define ANO_Flags     4003
#define NSF_NODUPS    1
#define NSF_CASE      2

struct nobj {
	APTR no_Object;                 /* the public part */
	struct nobj *next, *space;      /* in its name space; which one */
	struct nobj *first;             /* the objects in ours, if any */
	int has_space, flags, pri, uses, removed;
	struct Message *remmsg;
	char name[];
};

static struct nobj root_space = { .has_space = 1 };

static struct nobj *ns_of(uint32_t a)
{
	struct nobj *o = a ? PTR(a) : &root_space;

	return o->has_space ? o : NULL;
}

static int ns_cmp(struct nobj *ns, const char *a, const char *b)
{
	return ns->flags & NSF_CASE ? strcmp(a, b) : strcasecmp(a, b);
}

static void u_AllocNamedObjectA(struct frame *f)
{
	const char *name = PTR(A(0));
	struct TagItem *t = PTR(A(1)), *x;
	uint32_t user = 0;
	struct nobj *o;

	if (!name) {
		RET(0);
		return;
	}
	o = amem_alloc(sizeof(*o) + strlen(name) + 1);
	if (!o) {
		RET(0);
		return;
	}
	strcpy(o->name, name);
	o->uses = 1;
	while ((x = next_tag(&t))) {
		switch (x->ti_Tag) {
		case ANO_NameSpace: o->has_space = x->ti_Data != 0; break;
		case ANO_UserSpace: user = x->ti_Data; break;
		case ANO_Priority: o->pri = (int8_t)x->ti_Data; break;
		case ANO_Flags: o->flags = x->ti_Data & 3; break;
		}
	}
	if (user)
		o->no_Object = APTR_OF(amem_alloc(user));
	RET(o);
}

static void u_FreeNamedObject(struct frame *f)
{
	struct nobj *o = PTR(A(0));

	if (!o)
		return;
	if (o->no_Object)
		amem_free(PTR(o->no_Object));
	amem_free(o);
}

static void u_AddNamedObject(struct frame *f)
{
	struct nobj *ns = ns_of(A(0)), *o = PTR(A(1)), **p;

	if (!ns || !o) {
		RET(DOSFALSE);
		return;
	}
	if (ns->flags & NSF_NODUPS)
		for (struct nobj *q = ns->first; q; q = q->next)
			if (!ns_cmp(ns, q->name, o->name)) {
				RET(DOSFALSE);
				return;
			}
	for (p = &ns->first; *p && (*p)->pri >= o->pri; p = &(*p)->next)
		;
	o->next = *p;
	*p = o;
	o->space = ns;
	o->removed = 0;
	RET(DOSTRUE);
}

static void unlink_obj(struct nobj *o)
{
	struct nobj **p;

	if (!o->space)
		return;
	for (p = &o->space->first; *p; p = &(*p)->next)
		if (*p == o) {
			*p = o->next;
			break;
		}
	o->space = NULL;
	o->next = NULL;
}

static void u_FindNamedObject(struct frame *f)
{
	struct nobj *ns = ns_of(A(0)), *last = PTR(A(2)), *o;
	const char *name = PTR(A(1));

	if (!ns) {
		RET(0);
		return;
	}
	for (o = last ? last->next : ns->first; o; o = o->next)
		if (!o->removed && (!name || !ns_cmp(ns, o->name, name))) {
			o->uses++;
			RET(o);
			return;
		}
	RET(0);
}

static void u_NamedObjectName(struct frame *f)
{
	struct nobj *o = PTR(A(0));

	RET(o ? APTR_OF(o->name) : 0);
}

/* the last use of a removed object: it leaves its name space */
static void obj_released(struct nobj *o)
{
	extern void exec_reply(struct Message *m);

	if (o->removed && o->uses <= 0) {
		unlink_obj(o);
		if (o->remmsg) {
			o->remmsg->mn_Node.ln_Name = APTR_OF(o);
			exec_reply(o->remmsg);
			o->remmsg = NULL;
		}
	}
}

static void u_ReleaseNamedObject(struct frame *f)
{
	struct nobj *o = PTR(A(0));

	if (!o)
		return;
	if (o->uses > 0)
		o->uses--;
	obj_released(o);
}

static void u_AttemptRemNamedObject(struct frame *f)
{
	struct nobj *o = PTR(A(0));

	if (!o || o->removed || o->uses > 1) {
		RET(DOSFALSE);
		return;
	}
	o->uses = 0;
	unlink_obj(o);
	RET(DOSTRUE);
}

static void u_RemNamedObject(struct frame *f)
{
	extern void exec_reply(struct Message *m);
	struct nobj *o = PTR(A(0));
	struct Message *m = PTR(A(1));

	if (!o)
		return;
	if (o->removed) {               /* someone else is removing it */
		if (m) {
			m->mn_Node.ln_Name = 0;
			exec_reply(m);
		}
		if (o->uses > 0)
			o->uses--;
		return;
	}
	o->removed = 1;
	o->remmsg = m;
	if (o->uses > 0)
		o->uses--;
	obj_released(o);
}

static const struct libimpl utility_impl[] = {
	{ "ToUpper", u_ToUpper }, { "ToLower", u_ToLower },
	{ "Stricmp", u_Stricmp }, { "Strnicmp", u_Strnicmp },
	{ "SMult32", u_SMult32 }, { "UMult32", u_UMult32 },
	{ "SMult64", u_SMult64 }, { "UMult64", u_UMult64 },
	{ "SDivMod32", u_SDivMod32 }, { "UDivMod32", u_UDivMod32 },
	{ "Amiga2Date", u_Amiga2Date }, { "Date2Amiga", u_Date2Amiga },
	{ "CheckDate", u_CheckDate },
	{ "NextTagItem", u_NextTagItem }, { "FindTagItem", u_FindTagItem },
	{ "GetTagData", u_GetTagData }, { "TagInArray", u_TagInArray },
	{ "PackBoolTags", u_PackBoolTags },
	{ "AllocateTagItems", u_AllocateTagItems },
	{ "FreeTagItems", u_FreeTagItems },
	{ "CloneTagItems", u_CloneTagItems },
	{ "CallHookPkt", u_CallHookPkt }, { "GetUniqueID", u_GetUniqueID },
	{ "AllocNamedObjectA", u_AllocNamedObjectA },
	{ "FreeNamedObject", u_FreeNamedObject },
	{ "AddNamedObject", u_AddNamedObject },
	{ "FindNamedObject", u_FindNamedObject },
	{ "NamedObjectName", u_NamedObjectName },
	{ "ReleaseNamedObject", u_ReleaseNamedObject },
	{ "AttemptRemNamedObject", u_AttemptRemNamedObject },
	{ "RemNamedObject", u_RemNamedObject },
	{ NULL, NULL }
};

static void utility_setup(struct Library *b) { UtilityBase = b; }

struct amilib lib_utility = {
	.name = "utility.library", .version = 40, .revision = 1,
	.fd = fd_utility, .impl = utility_impl,
	.possize = 8, .init = utility_setup,
};

/* version.library: no functions, just the Workbench version */
struct amilib lib_version = {
	.name = "version.library", .version = 40, .revision = 42,
};
