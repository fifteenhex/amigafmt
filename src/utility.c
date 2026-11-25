/*
 * utility.library: tags, hooks, date conversion, 32/64-bit arithmetic
 * (SAS/C-compiled programs such as the 3.1 C: commands call UMult32 and
 * friends for every multiply and divide), and Latin-1 case handling.
 * Plus version.library, which only exists to carry a version number.
 */
#include <string.h>
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

	memset(&r, 0, sizeof(r));
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
