/*
 * Custom screens, colour maps and the display database.
 *
 * A screen of the program's own (OpenScreen/OpenScreenTagList) is a wbde
 * screen: its own size, depth and palette, and wbde shows whichever screen
 * is in front, scaled to the monitor (see "Screens" in wbde's
 * doc/client-protocol.md).  The screen's own pixels - what its RastPort
 * draws, under the windows - are a shared buffer of pens like a window's,
 * and windows opened on it (WA_CustomScreen, NW_CUSTOMSCREEN) are wbde
 * windows on that screen.  Its palette is the ViewPort's ColorMap: every
 * SetRGB4/SetRGB32/LoadRGB4/LoadRGB32 on it goes to wbde at the next flush.
 *
 * How big a screen pixel is on the monitor is wbde's choice by default:
 * the largest whole scale at which the screen fits the monitor, the same
 * on both axes so pixels stay square, centred with black round it; a
 * 320x256 screen is 1280x1024 on a 1920x1200 panel.
 * AMIGAFMT_SCREENSCALE=1 asks for 1:1, =modeid for the ModeID's own
 * whole numbers (low resolution pixels two display pixels wide, high one,
 * super-high half, rows doubled unless interlaced).
 *
 * The display database knows the NTSC and PAL modes an AGA Amiga has
 * except HAM (wbde screens are pens: HAM would need true colour), up to
 * 256 colours; DEFAULT_MONITOR_ID is NTSC.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include "gui.h"

#define TAG_USER        0x80000000u
#define SA_Dummy        (TAG_USER + 32)
#define NS_EXTENDED     0x1000
#define CUSTOMSCREEN    0x000F
#define PUBLICSCREEN    0x0002
#define SHOWTITLE       0x0010
#define CUSTOMBITMAP    0x0040
#define SCREENBEHIND    0x0080
#define SCREENQUIET     0x0100
#define OSERR_NOMONITOR 1
#define OSERR_NOCHIPS   2
#define OSERR_NOMEM     3
#define OSERR_UNKNOWNMODE 6
#define OSERR_TOODEEP   9
#define OSERR_NOTAVAILABLE 11

#define INVALID_ID      0xffffffffu
#define MONITOR_ID_MASK 0xffff1000u
#define NTSC_MONITOR_ID 0x00011000u
#define PAL_MONITOR_ID  0x00021000u
#define LACE            0x0004
#define SUPERHIRES      0x0020
#define EXTRA_HALFBRITE 0x0080
#define HAM             0x0800
#define HIRES           0x8000

struct ascreen *gui_screens;
extern struct Library *IntuitionBase;
extern struct TextFont *diskfont_open(const char *name, int ysize);
extern struct TextFont *diskfont_find(const char *name, int ysize, int exact);
extern struct TagItem *util_next_tag(struct TagItem **tp);

/* ---- the display database ---- */

struct mode {
	uint32_t key;           /* without the monitor */
	const char *name;
};

static const struct mode modes[] = {
	{ 0x0000, "Low Res" }, { 0x0004, "Low Res Laced" },
	{ 0x8000, "High Res" }, { 0x8004, "High Res Laced" },
	{ 0x8020, "Super-High Res" }, { 0x8024, "Super-High Res Laced" },
	{ 0x0080, "Extra-Half Brite" }, { 0x0084, "Extra-Half Brite Laced" },
};
#define NMODES (int)(sizeof(modes) / sizeof(modes[0]))
static const uint32_t monitors[] = { NTSC_MONITOR_ID, PAL_MONITOR_ID };

static uint32_t monitor_of(uint32_t id)
{
	uint32_t m = id & MONITOR_ID_MASK & ~0x1000u;

	if (!m)
		return NTSC_MONITOR_ID;         /* DEFAULT_MONITOR_ID */
	return id & MONITOR_ID_MASK;
}

/* is it one of ours?  0 if not */
int mode_known(uint32_t id)
{
	uint32_t mon = monitor_of(id), key = id & ~MONITOR_ID_MASK;
	int i;

	if (mon != NTSC_MONITOR_ID && mon != PAL_MONITOR_ID)
		return 0;
	for (i = 0; i < NMODES; i++)
		if (modes[i].key == key)
			return 1;
	return 0;
}

/* the nominal size of a mode */
void mode_size(uint32_t id, int *w, int *h)
{
	int pal = monitor_of(id) == PAL_MONITOR_ID;

	*w = (id & SUPERHIRES) ? 1280 : (id & HIRES) ? 640 : 320;
	*h = pal ? 256 : 200;
	if (id & LACE)
		*h *= 2;
}

/* display pixels a screen pixel takes, as num/den (see the top) */
void mode_scale(uint32_t id, int *sxn, int *sxd, int *syn, int *syd)
{
	if (!mode_known(id)) {
		*sxn = *sxd = *syn = *syd = 1;
		return;
	}
	*sxd = 1;
	*sxn = (id & SUPERHIRES) ? 1 : (id & HIRES) ? 1 : 2;
	if ((id & SUPERHIRES) && (id & HIRES))
		*sxd = 2;
	*syn = (id & LACE) ? 1 : 2;
	*syd = 1;
}

static int mode_depth(uint32_t id)
{
	return (id & EXTRA_HALFBRITE) ? 6 : 8;
}

static void g_BestModeIDA(struct frame *f)
{
	struct TagItem *t = PTR(A(0)), *x;
	uint32_t want_w = 640, want_h = 200, depth = 1, mon = NTSC_MONITOR_ID;
	uint32_t best = INVALID_ID;
	int bd = 1 << 30, i;

	uint32_t must = 0, mustnot = 0;

	while ((x = util_next_tag(&t))) {
		switch (x->ti_Tag - TAG_USER) {
		case 1: must = x->ti_Data; break;                       /* BIDTAG_DIPFMustHave */
		case 2: mustnot = x->ti_Data; break;                    /* BIDTAG_DIPFMustNotHave */
		case 3:                                                 /* BIDTAG_ViewPort */
			if (x->ti_Data) {
				struct ViewPort *vp = PTR(x->ti_Data);
				want_w = vp->DWidth;
				want_h = vp->DHeight;
			}
			break;
		case 4: case 6: want_w = x->ti_Data; break;             /* Nominal/DesiredWidth */
		case 5: case 7: want_h = x->ti_Data; break;             /* Nominal/DesiredHeight */
		case 8: depth = x->ti_Data; break;                      /* BIDTAG_Depth */
		case 9: mon = monitor_of(x->ti_Data); break;            /* BIDTAG_MonitorID */
		case 10: mon = monitor_of(x->ti_Data); break;           /* BIDTAG_SourceID */
		}
	}
	/* HAM is the only property we cannot give; EHB is ours if asked */
	if (must & 0x00000008) {
		RET(INVALID_ID);
		return;
	}
	for (i = 0; i < NMODES; i++) {
		uint32_t id = mon | modes[i].key;
		int w, h, d;
		if ((int)depth > mode_depth(id))
			continue;
		if (!!(id & EXTRA_HALFBRITE) != !!(must & 0x00001000))
			continue;
		if ((mustnot & 0x00000001) && (id & LACE))
			continue;
		mode_size(id, &w, &h);
		d = abs(w - (int)want_w) + abs(h - (int)want_h);
		if (d < bd) {
			bd = d;
			best = id;
		}
	}
	RET(best);
}

static void g_FindDisplayInfo(struct frame *f)
{
	static uint32_t handles[2 * NMODES];
	int i, m;

	for (m = 0; m < 2; m++)
		for (i = 0; i < NMODES; i++)
			if ((monitors[m] | modes[i].key) == D(0) ||
			    ((D(0) & MONITOR_ID_MASK) == 0 && m == 0 && modes[i].key == D(0))) {
				handles[m * NMODES + i] = D(0);
				RET(&handles[m * NMODES + i]);
				return;
			}
	RET(0);
}

static void g_NextDisplayInfo(struct frame *f)
{
	uint32_t last = D(0);
	int i, m, found = last == INVALID_ID;

	for (m = 0; m < 2; m++)
		for (i = 0; i < NMODES; i++) {
			uint32_t id = monitors[m] | modes[i].key;
			if (found) {
				RET(id);
				return;
			}
			if (id == last)
				found = 1;
		}
	RET(INVALID_ID);
}

static uint8_t *monitor_spec(int pal);

static void g_GetDisplayInfoData(struct frame *f)
{
	uint8_t *b = PTR(A(1));
	uint32_t size = D(0), tag = D(1), id = D(2), n;
	int w, h, pal;

	if (A(0) && id == INVALID_ID)
		id = *(uint32_t *)PTR(A(0));
	if (!mode_known(id)) {
		RET(0);
		return;
	}
	pal = monitor_of(id) == PAL_MONITOR_ID;
	mode_size(id, &w, &h);
	memset(b, 0, size);
	if (size < 16) {
		RET(0);
		return;
	}
	*(uint32_t *)(b + 0) = tag;
	*(uint32_t *)(b + 4) = id;
	*(uint32_t *)(b + 8) = 0;               /* SkipID: TAG_SKIP */
	switch (tag) {
	case 0x80000000:                        /* DTAG_DISP: DisplayInfo */
		n = 56;
		if (size >= 48) {
			uint32_t pf = 0x00000040 | 0x00010000 | 0x00002000;  /* SPRITES, AA, SPRITES_ATT */
			if (id & LACE) pf |= 0x00000001;
			if (pal) pf |= 0x00000020;
			if (id & EXTRA_HALFBRITE) pf |= 0x00001000;
			if ((id & HIRES) && !(id & SUPERHIRES)) pf |= 0x00000100;  /* DIPF_IS_WB */
			if (id & SUPERHIRES) pf |= 0x00000010;                     /* ECS */
			*(uint16_t *)(b + 16) = 0;      /* NotAvailable */
			*(uint32_t *)(b + 18) = pf;
			*(int16_t *)(b + 22) = (id & SUPERHIRES) ? 11 : (id & HIRES) ? 22 : 44;
			*(int16_t *)(b + 24) = (id & LACE) ? 22 : 44;
			*(uint16_t *)(b + 26) = (id & SUPERHIRES) ? 35 : (id & HIRES) ? 70 : 140;
			*(uint16_t *)(b + 28) = 8;
			*(uint16_t *)(b + 30) = 4096;
			*(int16_t *)(b + 32) = 22;
			*(int16_t *)(b + 34) = (id & LACE) ? 22 : 44;
			b[40] = b[41] = b[42] = 8;
		}
		break;
	case 0x80001000:                        /* DTAG_DIMS: DimensionInfo */
		n = 88;
		if (size >= 66) {
			int16_t *r = (int16_t *)(b + 26);
			int i;
			*(uint16_t *)(b + 16) = mode_depth(id);
			*(uint16_t *)(b + 18) = 16;
			*(uint16_t *)(b + 20) = 1;
			*(uint16_t *)(b + 22) = 16368;
			*(uint16_t *)(b + 24) = 16384;
			for (i = 0; i < 5; i++) {
				r[4 * i] = 0;
				r[4 * i + 1] = 0;
				r[4 * i + 2] = w - 1;
				r[4 * i + 3] = h - 1;
			}
		}
		break;
	case 0x80002000:                        /* DTAG_MNTR: MonitorInfo */
		n = 96;
		if (size >= 88) {
			*(int16_t *)(b + 24) = 22;      /* ViewResolution */
			*(int16_t *)(b + 26) = 22;
			*(uint16_t *)(b + 36) = pal ? 312 : 262;      /* TotalRows */
			*(uint16_t *)(b + 38) = 227;                  /* TotalColorClocks */
			*(uint16_t *)(b + 40) = pal ? 29 : 21;        /* MinRow */
			*(int16_t *)(b + 42) = 1;                     /* MCOMPAT_SELF */
			*(int16_t *)(b + 76) = 1;
			*(int16_t *)(b + 78) = 1;
			*(uint32_t *)(b + 84) = monitor_of(id) | HIRES;
			*(uint32_t *)(b + 16) = APTR_OF(monitor_spec(pal));    /* Mspc */
		}
		break;
	case 0x80003000:                        /* DTAG_NAME: NameInfo */
		n = 56;
		if (size >= 48) {
			int i;
			for (i = 0; i < NMODES; i++)
				if (modes[i].key == (id & ~MONITOR_ID_MASK))
					snprintf((char *)b + 16, 32, "%s:%s",
						 pal ? "PAL" : "NTSC", modes[i].name);
		}
		break;
	default:
		RET(0);
		return;
	}
	*(uint32_t *)(b + 12) = n / 8;          /* Length, in double longs */
	RET(n < size ? n : size);
}

/* for asl: the modes in order, and a mode's NameInfo and DimensionInfo */
uint32_t gfx_next_mode(uint32_t last)
{
	int i, m, found = last == INVALID_ID;

	for (m = 0; m < 2; m++)
		for (i = 0; i < NMODES; i++) {
			uint32_t id = monitors[m] | modes[i].key;
			if (found)
				return id;
			if (id == last)
				found = 1;
		}
	return INVALID_ID;
}

int gfx_mode_info(uint32_t id, uint8_t *name, uint8_t *dims)
{
	struct frame f;

	memset(&f, 0, sizeof(f));
	f.a[1] = APTR_OF(name);
	f.d[0] = 56;
	f.d[1] = 0x80003000;
	f.d[2] = id;
	g_GetDisplayInfoData(&f);
	if (!f.d[0])
		return 0;
	f.a[1] = APTR_OF(dims);
	f.d[0] = 88;
	f.d[1] = 0x80001000;
	f.d[2] = id;
	g_GetDisplayInfoData(&f);
	return f.d[0] != 0;
}

/*
 * OpenMonitor(name, displayID): a MonitorSpec (graphics/monitor.h) for
 * ntsc.monitor or pal.monitor, enough for programs that read its ratios,
 * row count and colour clocks.  160 bytes; one each, never freed.
 */
static uint8_t *ms[2];
static uint8_t *make_spec(int pal);

/* GfxBase->MonitorList (0x180) with both, default_monitor (0x18e) NTSC,
   DisplayFlags (0xce) NTSC: offsets from the NDK's graphics/gfxbase.h */
void gfx_monitors_init(void)
{
	extern void exec_addtail(void *l, void *n);
	struct List *l = (struct List *)((uint8_t *)GfxBase + 0x180);

	if (ms[0] || !GfxBase)
		return;
	l->lh_Head = APTR_OF(&l->lh_Tail);
	l->lh_Tail = 0;
	l->lh_TailPred = APTR_OF(&l->lh_Head);
	ms[0] = make_spec(0);
	ms[1] = make_spec(1);
	exec_addtail(l, ms[0]);
	exec_addtail(l, ms[1]);
	*(uint32_t *)((uint8_t *)GfxBase + 0x18e) = APTR_OF(ms[0]);
	*(uint16_t *)((uint8_t *)GfxBase + 0xce) = 1;          /* NTSC */
}

static uint8_t *monitor_spec(int pal)
{
	gfx_monitors_init();
	return ms[pal];
}

static uint8_t *make_spec(int pal)
{
	uint8_t *m;

	m = amem_alloc(160);
	m[8] = 0;                                       /* ln_Type */
	*(uint32_t *)(m + 10) = APTR_OF(pal ? "pal.monitor" : "ntsc.monitor");
	*(int32_t *)(m + 26) = 16;                      /* ratioh: RATIO_UNITY */
	*(int32_t *)(m + 30) = 16;                      /* ratiov */
	*(uint16_t *)(m + 34) = pal ? 312 : 262;        /* total_rows */
	*(uint16_t *)(m + 36) = 227;                    /* total_colorclocks */
	*(uint16_t *)(m + 38) = 0xd8;                   /* DeniseMaxDisplayColumn */
	*(uint16_t *)(m + 40) = pal ? 0x20 : 0;         /* BeamCon0: PAL */
	*(uint16_t *)(m + 42) = pal ? 29 : 21;          /* min_row */
	*(int16_t *)(m + 70) = 0x7fff;                  /* ms_LegalView */
	*(int16_t *)(m + 72) = 0x7fff;
	*(uint16_t *)(m + 82) = 0x18;                   /* DeniseMinDisplayColumn */
	*(uint32_t *)(m + 84) = 1;                      /* MCOMPAT_SELF */
	return m;
}

static void g_OpenMonitor(struct frame *f)
{
	const char *n = A(1) ? PTR(A(1)) : NULL;
	int pal = n ? !strncasecmp(n, "pal", 3) : monitor_of(D(0)) == PAL_MONITOR_ID;

	RET(monitor_spec(pal));
}

static void g_CloseMonitor(struct frame *f) { RET(0); }

static void g_ModeNotAvailable(struct frame *f)
{
	RET(mode_known(D(0)) ? 0 : 0x0001);    /* DI_AVAIL_NOCHIPS */
}

/* ---- colour maps ---- */

/*
 * Every ColorMap made here (GetColorMap, a screen's) is a real one, with
 * the 4-bit ColorTable and LowColorBits programs read, and its 8-bit
 * colours kept on this side.  The Workbench's colours are wbde's.
 */
struct acm {
	uint32_t addr;
	int count;
	uint8_t rgb[256][3];
	struct ascreen *as;
	struct acm *next;
};
static struct acm *acms;

static struct acm *acm_of(uint32_t addr)
{
	struct acm *c;

	for (c = acms; c; c = c->next)
		if (c->addr == addr)
			return c;
	return NULL;
}

static uint8_t *new_colormap(int count, struct acm **out)
{
	uint8_t *cm = amem_alloc(52);
	struct acm *c = calloc(1, sizeof(*c));

	if (count < 1)
		count = 1;
	if (count > 256)
		count = 256;
	cm[1] = 2;                              /* COLORMAP_TYPE_V39 */
	*(uint16_t *)(cm + 2) = count;
	*(uint32_t *)(cm + 4) = APTR_OF(amem_alloc(count * 2));   /* ColorTable */
	*(uint32_t *)(cm + 12) = APTR_OF(amem_alloc(count * 2));  /* LowColorBits */
	c->addr = APTR_OF(cm);
	c->count = count;
	c->next = acms;
	acms = c;
	if (out)
		*out = c;
	return cm;
}

static void acm_set(struct acm *c, int n, uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t *cm = PTR(c->addr);
	uint16_t *hi = PTR(*(uint32_t *)(cm + 4)), *lo = PTR(*(uint32_t *)(cm + 12));

	if (n < 0 || n >= c->count)
		return;
	c->rgb[n][0] = r;
	c->rgb[n][1] = g;
	c->rgb[n][2] = b;
	hi[n] = (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
	lo[n] = (r & 15) << 8 | (g & 15) << 4 | (b & 15);
	if (c->as) {
		struct ascreen *as = c->as;
		memcpy(as->pal[n], c->rgb[n], 3);
		if (as->pal_lo < 0 || n < as->pal_lo)
			as->pal_lo = n;
		if (n > as->pal_hi)
			as->pal_hi = n;
	}
}

/* colour n of a colour map: ours, else the Workbench's from wbde */
static const uint8_t *cm_rgb(uint32_t cm, int n)
{
	struct acm *c = acm_of(cm);
	static const uint8_t black[3];

	if (c)
		return n >= 0 && n < c->count ? c->rgb[n] : black;
	gui_connect();
	return gui_screen.palette[n & 255];
}

static int cm_count(uint32_t cm)
{
	struct acm *c = acm_of(cm);

	return c ? c->count : 256;
}

static uint32_t vp_cm(uint32_t vp)
{
	return vp ? ((struct ViewPort *)PTR(vp))->ColorMap : 0;
}

static void set_rgb(uint32_t cm, int n, uint8_t r, uint8_t g, uint8_t b)
{
	struct acm *c = acm_of(cm);

	if (c)
		acm_set(c, n, r, g, b);
	/* the Workbench's palette is wbde's to keep */
}

static void g_GetColorMap(struct frame *f)
{
	struct acm *c;
	uint8_t *cm = new_colormap(D(0), &c);
	int i;

	gui_connect();
	for (i = 0; i < c->count; i++)
		acm_set(c, i, gui_screen.palette[i][0], gui_screen.palette[i][1],
			gui_screen.palette[i][2]);
	RET(cm);
}

static void g_SetRGB4(struct frame *f)
{
	set_rgb(vp_cm(A(0)), D(0), (D(1) & 15) * 17, (D(2) & 15) * 17, (D(3) & 15) * 17);
}

static void g_SetRGB4CM(struct frame *f)
{
	set_rgb(A(0), D(0), (D(1) & 15) * 17, (D(2) & 15) * 17, (D(3) & 15) * 17);
}

static void g_SetRGB32(struct frame *f)
{
	set_rgb(vp_cm(A(0)), D(0), D(1) >> 24, D(2) >> 24, D(3) >> 24);
}

static void g_SetRGB32CM(struct frame *f)
{
	set_rgb(A(0), D(0), D(1) >> 24, D(2) >> 24, D(3) >> 24);
}

static void g_LoadRGB4(struct frame *f)
{
	const uint16_t *c = PTR(A(1));
	uint32_t cm = vp_cm(A(0));
	int i;

	for (i = 0; i < (int16_t)D(0); i++)
		set_rgb(cm, i, (c[i] >> 8 & 15) * 17, (c[i] >> 4 & 15) * 17, (c[i] & 15) * 17);
}

/* table: (count << 16 | first), then count * 3 longs, ...; count 0 ends */
static void g_LoadRGB32(struct frame *f)
{
	const uint32_t *t = PTR(A(1));
	uint32_t cm = vp_cm(A(0));

	while (t && *t >> 16) {
		int n = *t >> 16, first = *t & 0xffff, i;
		t++;
		for (i = 0; i < n; i++, t += 3)
			set_rgb(cm, first + i, t[0] >> 24, t[1] >> 24, t[2] >> 24);
	}
}

static void g_GetRGB4(struct frame *f)
{
	const uint8_t *c = cm_rgb(A(0), D(0));

	RET((c[0] >> 4) << 8 | (c[1] >> 4) << 4 | c[2] >> 4);
}

static void g_GetRGB32(struct frame *f)
{
	uint32_t *t = PTR(A(1)), i;

	for (i = 0; i < D(1); i++) {
		const uint8_t *c = cm_rgb(A(0), D(0) + i);
		t[3 * i] = c[0] * 0x01010101u;
		t[3 * i + 1] = c[1] * 0x01010101u;
		t[3 * i + 2] = c[2] * 0x01010101u;
	}
}

static int nearest(uint32_t cm, uint8_t r, uint8_t g, uint8_t b, int max)
{
	int best = 0, i, n = cm_count(cm);
	long bd = 1L << 30;

	if (max < 0 || max >= n)
		max = n - 1;
	for (i = 0; i <= max; i++) {
		const uint8_t *c = cm_rgb(cm, i);
		long dr = c[0] - r, dg = c[1] - g, db = c[2] - b;
		long d = dr * dr + dg * dg + db * db;
		if (d < bd)
			bd = d, best = i;
	}
	return best;
}

static void g_FindColor(struct frame *f)
{
	RET(nearest(A(3), D(1) >> 24, D(2) >> 24, D(3) >> 24, (int32_t)D(4)));
}

/* a custom screen's pens are all its owner's: the nearest colour; the
   Workbench's are shared through wbde */
static void g_ObtainBestPenA(struct frame *f)
{
	if (acm_of(A(0))) {
		RET(nearest(A(0), D(1) >> 24, D(2) >> 24, D(3) >> 24, -1));
		return;
	}
	RET(gui_obtain_pen(D(1) >> 24, D(2) >> 24, D(3) >> 24));
}

static void g_ObtainPen(struct frame *f)
{
	struct acm *c = acm_of(A(0));
	int32_t n = D(0);

	if (c) {
		if (n < 0 || n >= c->count) {
			RET(-1);
			return;
		}
		if (!(D(4) & 0x02))             /* not PEN_NO_SETCOLOR */
			acm_set(c, n, D(1) >> 24, D(2) >> 24, D(3) >> 24);
		RET(n);
		return;
	}
	if (n != -1) {
		RET(-1);        /* a particular pen: the Workbench keeps them */
		return;
	}
	RET(gui_obtain_pen(D(1) >> 24, D(2) >> 24, D(3) >> 24));
}

static void g_FreeColorMap(struct frame *f) { (void)f; }

static void g_GetVPModeID(struct frame *f)
{
	struct ascreen *as;

	for (as = gui_screens; as; as = as->next)
		if (APTR_OF(&as->scr->ViewPort) == A(0)) {
			RET(as->modeid);
			return;
		}
	RET(NTSC_MONITOR_ID | HIRES);
}

/*
 * The copper and View side of graphics: wbde builds the display, so
 * there is nothing to make, merge, load or free.  MakeVPort/MrgCop
 * succeed, VideoControl accepts its tags (0 = done).
 */
static void g_DispZero(struct frame *f) { RET(0); }
static void g_DispNop(struct frame *f) { (void)f; }

const struct libimpl screen_gfx_impl[] = {
	{ "FreeVPortCopLists", g_DispNop }, { "FreeCprList", g_DispNop },
	{ "FreeCopList", g_DispNop }, { "MakeVPort", g_DispZero },
	{ "MrgCop", g_DispZero }, { "LoadView", g_DispNop },
	{ "InitVPort", g_DispNop }, { "InitView", g_DispNop },
	{ "CBump", g_DispNop }, { "CMove", g_DispNop }, { "CWait", g_DispNop },
	{ "VideoControl", g_DispZero }, { "ScrollVPort", g_DispNop },
	{ "BestModeIDA", g_BestModeIDA }, { "FindDisplayInfo", g_FindDisplayInfo },
	{ "NextDisplayInfo", g_NextDisplayInfo },
	{ "GetDisplayInfoData", g_GetDisplayInfoData },
	{ "ModeNotAvailable", g_ModeNotAvailable },
	{ "OpenMonitor", g_OpenMonitor }, { "CloseMonitor", g_CloseMonitor },
	{ "GetColorMap", g_GetColorMap }, { "FreeColorMap", g_FreeColorMap },
	{ "SetRGB4", g_SetRGB4 }, { "SetRGB4CM", g_SetRGB4CM },
	{ "SetRGB32", g_SetRGB32 }, { "SetRGB32CM", g_SetRGB32CM },
	{ "LoadRGB4", g_LoadRGB4 }, { "LoadRGB32", g_LoadRGB32 },
	{ "GetRGB4", g_GetRGB4 }, { "GetRGB32", g_GetRGB32 },
	{ "FindColor", g_FindColor }, { "ObtainBestPenA", g_ObtainBestPenA },
	{ "ObtainPen", g_ObtainPen }, { "GetVPModeID", g_GetVPModeID },
	{ NULL, NULL }
};

/* ---- screens ---- */

struct ascreen *as_by_screen(uint32_t scr)
{
	struct ascreen *as;

	for (as = gui_screens; as; as = as->next)
		if (APTR_OF(as->scr) == scr)
			return as;
	return NULL;
}

/* a RastPort drawing on a custom screen's own pixels */
struct ascreen *as_by_rp(struct RastPort *rp)
{
	struct ascreen *as;

	for (as = gui_screens; as; as = as->next)
		if (rp == &as->scr->RastPort ||
		    (rp->BitMap && rp->BitMap == APTR_OF(&as->scr->BitMap)))
			return as;
	return NULL;
}

void as_damage(struct ascreen *as, int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= as->w) x1 = as->w - 1;
	if (y1 >= as->h) y1 = as->h - 1;
	if (x0 > x1 || y0 > y1)
		return;
	if (!as->dirty) {
		as->dirty = 1;
		as->dx0 = x0, as->dy0 = y0, as->dx1 = x1, as->dy1 = y1;
		return;
	}
	if (x0 < as->dx0) as->dx0 = x0;
	if (y0 < as->dy0) as->dy0 = y0;
	if (x1 > as->dx1) as->dx1 = x1;
	if (y1 > as->dy1) as->dy1 = y1;
}

/*
 * A write to a protected plane page (main.c's fault handler asks): the
 * rows that page holds are to be converted, and the page is writable
 * until the next flush.  Signal-safe: arithmetic and mprotect only.
 */
unsigned long screen_plane_faults;

int screen_plane_fault(uint32_t addr)
{
	struct ascreen *as;
	int p;

	screen_plane_faults++;
	for (as = gui_screens; as; as = as->next)
		for (p = 0; p < as->depth; p++) {
			uint32_t base = APTR_OF(as->planes[p]);
			if (base && addr >= base && addr < base + as->planesize) {
				uint32_t page = (addr - base) & ~4095u;
				int y0 = page / as->bpr, y1 = (page + 4095) / as->bpr;
				if (y1 >= as->h)
					y1 = as->h - 1;
				if (y0 < as->conv_y0)
					as->conv_y0 = y0;
				if (y1 > as->conv_y1)
					as->conv_y1 = y1;
				mprotect(as->planes[p] + page, 4096, PROT_READ | PROT_WRITE);
				return 1;
			}
		}
	return 0;
}

/* graphics drew rows y0..y1 (through the alias) */
void as_rows_drawn(struct ascreen *as, int y0, int y1)
{
	if (y0 < as->conv_y0)
		as->conv_y0 = y0;
	if (y1 > as->conv_y1)
		as->conv_y1 = y1;
}

/* planar rows y0..y1 into the screen's buffer of pens, protected again */
static void convert(struct ascreen *as)
{
	int y0 = as->conv_y0, y1 = as->conv_y1, y, bx, p, d = as->depth;

	if (y0 > y1)
		return;
	as->conv_y0 = 1 << 30;
	as->conv_y1 = -1;
	mprotect(as->planes[0], as->planesize * d, PROT_READ);
	for (y = y0; y <= y1; y++) {
		uint8_t *o = as->pix + (size_t)y * as->stride;
		const uint8_t *row[8];
		for (p = 0; p < d; p++)
			row[p] = as->alias[p] + (size_t)y * as->bpr;
		for (bx = 0; bx < as->bpr && bx * 8 < as->w; bx++) {
			uint8_t px[8] = { 0 };
			int b, n = as->w - bx * 8 < 8 ? as->w - bx * 8 : 8;
			for (p = 0; p < d; p++) {
				uint8_t v = row[p][bx];
				if (!v)
					continue;
				for (b = 0; b < 8; b++)
					px[b] |= ((v >> (7 - b)) & 1) << p;
			}
			memcpy(o + bx * 8, px, n);
		}
	}
	as_damage(as, 0, y0, as->w - 1, y1);
}

/* the screens' damage and palette changes to wbde, before a wait */
void as_flush(void)
{
	struct ascreen *as;

	for (as = gui_screens; as; as = as->next) {
		convert(as);
		if (as->pal_lo >= 0) {
			int n = as->pal_hi - as->pal_lo + 1;
			uint8_t buf[4 + 256 * 3];
			struct wbp_palette *wp = (struct wbp_palette *)buf;

			wp->first = as->pal_lo;
			wp->count = n;
			memcpy(buf + 4, as->pal[as->pal_lo], n * 3);
			gui_send(WBP_SCREEN_PALETTE, as->id, buf, 4 + n * 3);
			as->pal_lo = as->pal_hi = -1;
		}
		if (as->dirty) {
			struct wbp_rect r = { as->dx0, as->dy0, as->dx1 - as->dx0 + 1,
					      as->dy1 - as->dy0 + 1 };
			as->dirty = 0;
			gui_send(WBP_SCREEN_DAMAGE, as->id, &r, sizeof(r));
		}
	}
}

/* IntuitionBase's list: FirstScreen, each NextScreen; ActiveScreen */
static void relink(void)
{
	struct ascreen *as;
	uint32_t *first = (uint32_t *)((uint8_t *)IntuitionBase + 60);
	uint32_t prev = 0;

	/* front to back: the custom screens in the order of the list, then
	   Workbench */
	*first = 0;
	for (as = gui_screens; as; as = as->next) {
		if (prev)
			((struct Screen *)PTR(prev))->NextScreen = APTR_OF(as->scr);
		else
			*first = APTR_OF(as->scr);
		prev = APTR_OF(as->scr);
	}
	if (wb_screen) {
		if (prev)
			((struct Screen *)PTR(prev))->NextScreen = APTR_OF(wb_screen);
		else
			*first = APTR_OF(wb_screen);
		wb_screen->NextScreen = 0;
	} else if (prev)
		((struct Screen *)PTR(prev))->NextScreen = 0;
}

static const uint16_t newlook[WBP_NUMPENS] = {
	0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1
};

/* the screen's font: what it asked for (memory, then disk), else topaz */
static struct TextFont *screen_font(struct TextAttr *ta)
{
	struct TextFont *tf = NULL;

	if (ta && ta->ta_Name) {
		tf = diskfont_find(PTR(ta->ta_Name), ta->ta_YSize, 1);
		if (!tf)
			tf = diskfont_open(PTR(ta->ta_Name), ta->ta_YSize);
	}
	return tf ? tf : gui_topaz;
}

struct sspec {
	int left, top, width, height, depth;
	int detail, block;
	uint32_t modeid;
	int type;
	uint32_t font, title, colors, colors32, pens, pubname, errcode, bitmap;
	int sysfont, showtitle, behind, quiet;
};

static void read_stags(struct sspec *s, struct TagItem *t)
{
	struct TagItem *x;

	while ((x = util_next_tag(&t))) {
		uint32_t d = x->ti_Data;
		switch (x->ti_Tag - SA_Dummy) {
		case 0x01: s->left = (int16_t)d; break;
		case 0x02: s->top = (int16_t)d; break;
		case 0x03: s->width = (int32_t)d; break;
		case 0x04: s->height = (int32_t)d; break;
		case 0x05: s->depth = d; break;
		case 0x06: s->detail = d; break;
		case 0x07: s->block = d; break;
		case 0x08: s->title = d; break;
		case 0x09: s->colors = d; break;
		case 0x0a: s->errcode = d; break;
		case 0x0b: s->font = d; break;
		case 0x0c: s->sysfont = d; break;
		case 0x0d: s->type = d; break;
		case 0x0e: s->bitmap = d; break;
		case 0x0f: s->pubname = d; break;
		case 0x12: s->modeid = d; break;
		case 0x16: s->showtitle = d != 0; break;
		case 0x17: s->behind = d != 0; break;
		case 0x18: s->quiet = d != 0; break;
		case 0x1a: s->pens = d; break;
		case 0x23: s->colors32 = d; break;
		case 0x27:                              /* SA_LikeWorkbench */
			if (d) {
				gui_connect();
				s->width = gui_screen.width;
				s->height = gui_screen.height;
				s->depth = gui_screen.depth > 8 ? 8 : gui_screen.depth;
				s->modeid = NTSC_MONITOR_ID | HIRES;
			}
			break;
		}
	}
}

static struct Screen *open_screen(struct sspec *sp)
{
	struct wbp_newscreen ns;
	struct wbp_screenbuf m;
	struct ascreen *as;
	struct Screen *s;
	struct acm *c;
	struct TextFont *tf;
	struct TextAttr *ta;
	uint16_t *pens;
	uint8_t *ri;
	int w = sp->width, h = sp->height, d = sp->depth, i, sxn, sxd, syn, syd, mfd = -1;
	uint32_t id = sp->modeid, sid;
	int err = 0;

	if (gui_connect()) {
		af_warn("%s: no wbde to open a screen on (WBDE_SOCKET)", af_progname);
		err = OSERR_NOMONITOR;
		goto fail;
	}
	if ((id & ~MONITOR_ID_MASK) & HAM) {
		af_warn("%s: HAM screens are not provided (ModeID $%x, %dx%d+%d+%d, depth %d)",
			af_progname, id, sp->width, sp->height, sp->left, sp->top, sp->depth);
		err = OSERR_UNKNOWNMODE;
		goto fail;
	}
	if (!mode_known(id)) {
		/* a mode of another display card: the nearest of ours */
		af_warn("%s: unknown ModeID $%x, using NTSC:High Res", af_progname, id);
		id = NTSC_MONITOR_ID | HIRES | (id & LACE);
	}
	if (d < 1)
		d = 1;
	if (d > mode_depth(id)) {
		err = OSERR_TOODEEP;
		goto fail;
	}
	{
		int nw, nh;
		mode_size(id, &nw, &nh);
		if (w <= 0)
			w = nw;
		if (h <= 0)
			h = nh;
	}
	/* wbde's choice (the largest whole scale that fits the monitor,
	   square pixels, centred); AMIGAFMT_SCREENSCALE=modeid asks for the
	   ModeID's own as described at the top, =1 for 1:1 */
	if (getenv("AMIGAFMT_SCREENSCALE") && !strcmp(getenv("AMIGAFMT_SCREENSCALE"), "modeid"))
		mode_scale(id, &sxn, &sxd, &syn, &syd);
	else if (getenv("AMIGAFMT_SCREENSCALE") && !strcmp(getenv("AMIGAFMT_SCREENSCALE"), "1"))
		sxn = sxd = syn = syd = 1;
	else
		sxn = syn = 0, sxd = syd = 1;

	memset(&ns, 0, sizeof(ns));
	ns.width = w;
	ns.height = h;
	ns.depth = d;
	ns.flags = sp->showtitle && !sp->quiet ? WBP_SF_SHOWTITLE : 0;
	ns.sx_num = sxn;
	ns.sx_den = sxd;
	ns.sy_num = syn;
	ns.sy_den = syd;
	pens = amem_alloc(2 * (WBP_NUMPENS + 1));
	for (i = 0; i < WBP_NUMPENS; i++)
		pens[i] = newlook[i];
	if (d == 1) {
		pens[WBP_FILLPEN] = 1;
		pens[WBP_SHINEPEN] = 1;
		pens[WBP_BARBLOCKPEN] = 1;
		pens[WBP_BARDETAILPEN] = 0;
		pens[WBP_HIGHLIGHTTEXTPEN] = 1;
	}
	if (sp->pens) {
		const uint16_t *p = PTR(sp->pens);
		for (i = 0; i < WBP_NUMPENS && p[i] != 0xffff; i++)
			pens[i] = p[i];
	} else {
		pens[WBP_DETAILPEN] = sp->detail;
		pens[WBP_BLOCKPEN] = sp->block;
	}
	pens[WBP_NUMPENS] = 0xffff;
	for (i = 0; i < WBP_NUMPENS; i++)
		ns.pens[i] = pens[i] & ((1 << d) - 1);
	if (sp->title)
		snprintf(ns.title, sizeof(ns.title), "%s", (char *)PTR(sp->title));

	as = calloc(1, sizeof(*as));
	as->pal_lo = as->pal_hi = -1;
	/* the palette: Workbench's to start with, then SA_Colors(32) */
	{
		uint8_t *cm = new_colormap(1 << d, &c);
		c->as = as;
		for (i = 0; i < (1 << d); i++)
			acm_set(c, i, gui_screen.palette[i][0], gui_screen.palette[i][1],
				gui_screen.palette[i][2]);
		if (sp->colors) {
			const int16_t *cs = PTR(sp->colors);
			for (; cs[0] != -1; cs += 4)
				acm_set(c, cs[0], (cs[1] & 15) * 17, (cs[2] & 15) * 17,
					(cs[3] & 15) * 17);
		}
		if (sp->colors32) {
			const uint32_t *t = PTR(sp->colors32);
			while (*t >> 16) {
				int n = *t >> 16, first = *t & 0xffff;
				t++;
				for (i = 0; i < n; i++, t += 3)
					acm_set(c, first + i, t[0] >> 24, t[1] >> 24, t[2] >> 24);
			}
		}
		as->cm = cm;
	}
	memcpy(ns.palette, as->pal, sizeof(ns.palette));
	as->pal_lo = as->pal_hi = -1;

	if (gui_call(WBP_OPEN_SCREEN, 0, &ns, sizeof(ns), WBP_SCREEN_OPENED, &m, sizeof(m),
		     &mfd, &sid) || mfd < 0) {
		free(as);
		err = OSERR_NOMEM;
		goto fail;
	}
	as->id = sid;
	as->w = m.width;
	as->h = m.height;
	as->stride = m.stride;
	as->size = m.size;
	as->depth = d;
	as->modeid = id;
	as->pix = gui_map(mfd, m.size);
	as->barh = m.barheight;
	as->wbortop = m.wbortop;

	/* the Screen */
	s = amem_alloc(sizeof(*s));
	as->scr = s;
	tf = sp->sysfont == 1 ? gui_topaz : screen_font(sp->font ? PTR(sp->font) : NULL);
	ta = amem_alloc(sizeof(*ta) + 64);
	ta->ta_Name = APTR_OF((char *)(ta + 1));
	snprintf((char *)(ta + 1), 64, "%s", tf->tf_Message.mn_Node.ln_Name ?
		 (char *)PTR(tf->tf_Message.mn_Node.ln_Name) : "topaz.font");
	if (!strstr((char *)(ta + 1), ".font"))
		strncat((char *)(ta + 1), ".font", 63 - strlen((char *)(ta + 1)));
	ta->ta_YSize = tf->tf_YSize;
	ta->ta_Style = tf->tf_Style;
	ta->ta_Flags = tf->tf_Flags;
	as->font = tf;
	s->LeftEdge = 0;
	s->TopEdge = 0;
	s->Width = w;
	s->Height = h;
	s->Flags = CUSTOMSCREEN | (sp->showtitle ? SHOWTITLE : 0) |
		   (sp->behind ? SCREENBEHIND : 0) | (sp->quiet ? SCREENQUIET : 0) |
		   (sp->bitmap ? CUSTOMBITMAP : 0);
	if (sp->pubname)
		s->Flags = (s->Flags & ~0x000F) | PUBLICSCREEN;
	s->Title = s->DefaultTitle = sp->title;
	s->BarHeight = m.barheight - 1;
	s->BarVBorder = 1;
	s->BarHBorder = 5;
	s->MenuVBorder = 2;
	s->MenuHBorder = 4;
	s->WBorTop = m.wbortop - (tf->tf_YSize + 1) > 0 ? m.wbortop - (tf->tf_YSize + 1) : 2;
	s->WBorLeft = 4;
	s->WBorRight = 4;
	s->WBorBottom = 2;
	s->Font = APTR_OF(ta);
	s->ViewPort.DWidth = w;
	s->ViewPort.DHeight = h;
	s->ViewPort.Modes = id & 0xffff & ~0x1000;
	s->ViewPort.ColorMap = APTR_OF(as->cm);
	*(uint32_t *)(as->cm + 20) = APTR_OF(&s->ViewPort);     /* cm_vp */
	*(uint32_t *)(as->cm + 36) = id;                         /* VPModeID */
	ri = amem_alloc(12);                                     /* RasInfo */
	*(uint32_t *)(ri + 4) = APTR_OF(&s->BitMap);
	s->ViewPort.RasInfo = APTR_OF(ri);
	if (sp->bitmap)
		memcpy(&s->BitMap, PTR(sp->bitmap), sizeof(s->BitMap));
	else {
		s->BitMap.BytesPerRow = ((w + 15) / 16) * 2;
		s->BitMap.Rows = h;
		s->BitMap.Depth = d;
		/* the planes are the screen's pixels (see as_flush) */
		as->bpr = s->BitMap.BytesPerRow;
		as->planesize = ((size_t)as->bpr * h + 4095) & ~(size_t)4095;
		as->conv_y0 = 1 << 30;
		as->conv_y1 = -1;
		/* one memfd mapped twice: the program's view, write protected
		   so that its writes are noticed, and amigafmt's, writable
		   (graphics draws there and says which rows itself) */
		{
			int fd = memfd_create("amigafmt-planes", MFD_CLOEXEC);
			size_t all = as->planesize * d;
			uint8_t *a = MAP_FAILED, *b = MAP_FAILED;

			if (fd >= 0 && !ftruncate(fd, all)) {
				a = mmap(NULL, all, PROT_READ, MAP_SHARED, fd, 0);
				b = mmap(NULL, all, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			}
			if (fd >= 0)
				close(fd);
			for (i = 0; i < d; i++) {
				as->planes[i] = a == MAP_FAILED || b == MAP_FAILED ? NULL :
						a + i * as->planesize;
				as->alias[i] = as->planes[i] ? b + i * as->planesize : NULL;
				s->BitMap.Planes[i] = APTR_OF(as->planes[i]);
			}
		}
	}
	gfx_init_rp(&s->RastPort);
	s->RastPort.BitMap = APTR_OF(&s->BitMap);
	s->RastPort.Font = APTR_OF(tf);
	s->RastPort.TxHeight = tf->tf_YSize;
	s->RastPort.TxWidth = tf->tf_XSize;
	s->RastPort.TxBaseline = tf->tf_Baseline;
	s->RastPort.FgPen = 1;
	s->DetailPen = pens[WBP_DETAILPEN];
	s->BlockPen = pens[WBP_BLOCKPEN];

	as->dri = amem_alloc(sizeof(*as->dri));
	as->dri->dri_Version = 2;
	as->dri->dri_NumPens = WBP_NUMPENS;
	as->dri->dri_Pens = APTR_OF(pens);
	as->dri->dri_Font = APTR_OF(tf);
	as->dri->dri_Depth = d;
	as->dri->dri_ResX = (id & HIRES) ? 22 : 44;
	as->dri->dri_ResY = (id & LACE) ? 22 : 44;
	as->dri->dri_Flags = 1;                 /* DRIF_NEWLOOK */
	as->pens = pens;
	if (sp->pubname) {
		snprintf(as->pubname, sizeof(as->pubname), "%s", (char *)PTR(sp->pubname));
		as->pub = 1;
	}

	as->next = gui_screens;
	gui_screens = as;
	relink();
	if (sp->behind)
		gui_send(WBP_SCREEN_TO_BACK, as->id, NULL, 0);
	*(uint32_t *)((uint8_t *)IntuitionBase + 56) = APTR_OF(s);  /* ActiveScreen */
	if (af_trace)
		fprintf(stderr, "amigafmt: screen %u: %dx%dx%d mode $%x, scale %d/%d x %d/%d, "
			"font %s/%d\n", as->id, w, h, d, id, sxn, sxd, syn, syd,
			(char *)(ta + 1), ta->ta_YSize);
	return s;
fail:
	if (sp->errcode)
		*(uint32_t *)PTR(sp->errcode) = err;
	return NULL;
}

static void open_common(struct frame *f, uint32_t tags)
{
	struct NewScreen {
		int16_t LeftEdge, TopEdge, Width, Height, Depth;
		uint8_t DetailPen, BlockPen;
		uint16_t ViewModes, Type;
		APTR Font, DefaultTitle, Gadgets, CustomBitMap;
		APTR Extension;                 /* ExtNewScreen */
	} *n = PTR(A(0));
	struct sspec s;

	memset(&s, 0, sizeof(s));
	s.modeid = INVALID_ID;
	s.showtitle = 1;
	s.block = 1;
	if (n) {
		s.left = n->LeftEdge;
		s.top = n->TopEdge;
		s.width = n->Width;
		s.height = n->Height;
		s.depth = n->Depth;
		s.detail = n->DetailPen;
		s.block = n->BlockPen;
		s.modeid = NTSC_MONITOR_ID |
			   (n->ViewModes & (HIRES | LACE | SUPERHIRES | HAM | EXTRA_HALFBRITE));
		s.type = n->Type;
		s.font = n->Font;
		s.title = n->DefaultTitle;
		if (n->Type & CUSTOMBITMAP)
			s.bitmap = n->CustomBitMap;
		if (n->Type & SCREENBEHIND)
			s.behind = 1;
		if (n->Type & SCREENQUIET)
			s.quiet = 1;
		if ((n->Type & NS_EXTENDED) && n->Extension)
			read_stags(&s, PTR(n->Extension));
	}
	if (tags)
		read_stags(&s, PTR(tags));
	if (s.modeid == INVALID_ID)
		s.modeid = NTSC_MONITOR_ID | HIRES;
	RET(open_screen(&s));
}

static void i_OpenScreen(struct frame *f)
{
	open_common(f, 0);
}

static void i_OpenScreenTagList(struct frame *f)
{
	open_common(f, A(1));
}

/* is the screen a custom one of ours; the Workbench answers 0 */
static void i_CloseScreen(struct frame *f)
{
	struct ascreen *as = as_by_screen(A(0)), **pp;

	if (!as) {
		RET(0);
		return;
	}
	if (as->scr->FirstWindow) {
		RET(0);                         /* Intuition refuses too */
		return;
	}
	gui_flush();
	gui_send(WBP_SCREEN_CLOSE, as->id, NULL, 0);
	for (pp = &gui_screens; *pp; pp = &(*pp)->next)
		if (*pp == as) {
			*pp = as->next;
			break;
		}
	gui_unmap(as->pix, as->size);
	{
		if (as->planes[0]) {
			munmap(as->planes[0], as->planesize * as->depth);
			munmap(as->alias[0], as->planesize * as->depth);
		}
	}
	relink();
	if (*(uint32_t *)((uint8_t *)IntuitionBase + 56) == APTR_OF(as->scr))
		*(uint32_t *)((uint8_t *)IntuitionBase + 56) =
			gui_screens ? APTR_OF(gui_screens->scr) : APTR_OF(wb_screen);
	free(as);
	RET(1);
}

static void to_front_list(struct ascreen *as)
{
	struct ascreen **pp;

	for (pp = &gui_screens; *pp; pp = &(*pp)->next)
		if (*pp == as) {
			*pp = as->next;
			break;
		}
	as->next = gui_screens;
	gui_screens = as;
	relink();
}

static void i_ScreenToFront(struct frame *f)
{
	struct ascreen *as = as_by_screen(A(0));

	if (as) {
		to_front_list(as);
		gui_send(WBP_SCREEN_TO_FRONT, as->id, NULL, 0);
	}
}

static void i_ScreenToBack(struct frame *f)
{
	struct ascreen *as = as_by_screen(A(0)), **pp;

	if (!as)
		return;
	for (pp = &gui_screens; *pp; pp = &(*pp)->next)
		if (*pp == as) {
			*pp = as->next;
			break;
		}
	for (pp = &gui_screens; *pp; pp = &(*pp)->next)
		;
	*pp = as;
	as->next = NULL;
	relink();
	gui_send(WBP_SCREEN_TO_BACK, as->id, NULL, 0);
}

void as_set_title(struct ascreen *as, const char *title, int show)
{
	char buf[164];
	int32_t s = show;
	size_t n = title ? strlen(title) : 0;

	if (n > sizeof(buf) - 5)
		n = sizeof(buf) - 5;
	memcpy(buf, &s, 4);
	if (n)
		memcpy(buf + 4, title, n);
	gui_send(WBP_SCREEN_TITLE, as->id, buf, 4 + n);
}

static void i_ShowTitle(struct frame *f)
{
	struct ascreen *as = as_by_screen(A(0));

	if (!as)
		return;
	as->scr->Flags = D(0) ? as->scr->Flags | SHOWTITLE : as->scr->Flags & ~SHOWTITLE;
	as_set_title(as, as->scr->Title ? PTR(as->scr->Title) : "", D(0) != 0);
}

static void i_LockPubScreen(struct frame *f)
{
	const char *n = A(0) ? PTR(A(0)) : NULL;
	struct ascreen *as;

	if (n && strcmp(n, "Workbench")) {
		for (as = gui_screens; as; as = as->next)
			if (as->pub && !strcmp(as->pubname, n)) {
				as->publocks++;
				RET(as->scr);
				return;
			}
		RET(0);
		return;
	}
	if (gui_connect()) {
		RET(0);
		return;
	}
	RET(wb_screen);
}

static void i_GetScreenDrawInfo(struct frame *f)
{
	struct ascreen *as = as_by_screen(A(0));

	if (as) {
		RET(as->dri);
		return;
	}
	RET(gui_connect() ? 0 : APTR_OF(gui_dri));
}

/* GetScreenData(buffer, size, type, screen) */
static void i_GetScreenData(struct frame *f)
{
	struct ascreen *as = (D(1) & 0x000F) == CUSTOMSCREEN ? as_by_screen(A(1)) : NULL;
	struct Screen *s = as ? as->scr : NULL;

	if (!s) {
		if (gui_connect()) {
			RET(0);
			return;
		}
		s = wb_screen;
	}
	memcpy(PTR(A(0)), s, D(0) < sizeof(struct Screen) ? D(0) : sizeof(struct Screen));
	RET(1);
}

static void i_ViewPortAddress(struct frame *f)
{
	struct Window *w = PTR(A(0));

	if (w && w->WScreen) {
		RET(&((struct Screen *)PTR(w->WScreen))->ViewPort);
		return;
	}
	RET(gui_connect() ? 0 : APTR_OF(&wb_screen->ViewPort));
}

/* PubScreenStatus(screen, flags): a private screen; ours are public when named */
static void i_PubScreenStatus(struct frame *f)
{
	RET(0);
}

static void i_UnlockPubScreen(struct frame *f)
{
	struct ascreen *as = A(1) ? as_by_screen(A(1)) : NULL;

	if (as && as->publocks)
		as->publocks--;
}

const struct libimpl screen_intuition_impl[] = {
	{ "OpenScreen", i_OpenScreen }, { "OpenScreenTagList", i_OpenScreenTagList },
	{ "CloseScreen", i_CloseScreen },
	{ "ScreenToFront", i_ScreenToFront }, { "ScreenToBack", i_ScreenToBack },
	{ "ShowTitle", i_ShowTitle }, { "LockPubScreen", i_LockPubScreen },
	{ "UnlockPubScreen", i_UnlockPubScreen },
	{ "GetScreenDrawInfo", i_GetScreenDrawInfo },
	{ "GetScreenData", i_GetScreenData },
	{ "ViewPortAddress", i_ViewPortAddress },
	{ "PubScreenStatus", i_PubScreenStatus },
	{ NULL, NULL }
};
