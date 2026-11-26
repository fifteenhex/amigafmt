/*
 * gadtools.library, drawn and run by amigafmt itself in the 3.x look.
 *
 * Every gadget is a real struct Gadget in the window's list (programs read
 * GadgetID, UserData, Flags & SELECTED, StringInfo buffers); what gadtools
 * keeps about it (kind, labels, values) is in a side table here, and
 * intuition.c asks us first before treating a gadget as plain Intuition.
 * STRING and INTEGER gadgets are Intuition string gadgets underneath.
 *
 * gadtools_lib.fd is not in amitools, so the vector table below is written
 * by hand from the NDK 3.1 gadtools_lib.fd (CreateGadgetA -30 ...
 * GT_GetGadgetAttrsA -174); tools/fdcheck.py checks it.
 */
#include <stdlib.h>
#include <string.h>
#include "gui.h"

#define GENERIC_KIND  0
#define BUTTON_KIND   1
#define CHECKBOX_KIND 2
#define INTEGER_KIND  3
#define LISTVIEW_KIND 4
#define MX_KIND       5
#define NUMBER_KIND   6
#define CYCLE_KIND    7
#define PALETTE_KIND  8
#define SCROLLER_KIND 9
#define SLIDER_KIND   11
#define STRING_KIND   12
#define TEXT_KIND     13

#define GT(n)          (0x80080000 + (n))
#define GTCB_Checked   GT(4)
#define GTLV_Top       GT(5)
#define GTLV_Labels    GT(6)
#define GTLV_ReadOnly  GT(7)
#define GTMX_Labels    GT(9)
#define GTMX_Active    GT(10)
#define GTTX_Text      GT(11)
#define GTTX_CopyText  GT(12)
#define GTNM_Number    GT(13)
#define GTCY_Labels    GT(14)
#define GTCY_Active    GT(15)
#define GTPA_Depth     GT(16)
#define GTPA_Color     GT(17)
#define GTPA_ColorOffset GT(18)
#define GTSC_Top       GT(21)
#define GTSC_Total     GT(22)
#define GTSC_Visible   GT(23)
#define GTSL_Min       GT(38)
#define GTSL_Max       GT(39)
#define GTSL_Level     GT(40)
#define GTST_String    GT(45)
#define GTST_MaxChars  GT(46)
#define GTIN_Number    GT(47)
#define GTIN_MaxChars  GT(48)
#define GTBB_Recessed  GT(51)
#define GTLV_ShowSelected GT(53)
#define GTLV_Selected  GT(54)
#define GTTX_Border    GT(57)
#define GTNM_Border    GT(58)
#define GTSC_Arrows    GT(59)
#define GT_Underscore  GT(64)
#define GTPA_NumColors GT(70)
#define GTMX_TitlePlace GT(71)
#define GTSL_MaxLevelLen GT(41)
#define GTSL_LevelFormat GT(42)
#define GTSL_LevelPlace GT(43)
#define GTSL_DispFunc  GT(44)
#define GTBB_FrameType GT(80)
#define GTLV_MakeVisible GT(81)
#define GA_Disabled    0x8003000e
#define GA_Immediate   0x80030015
#define GA_RelVerify   0x80030016
#define PGA_Freedom    0x80031001

#define PLACETEXT_LEFT  0x01
#define PLACETEXT_RIGHT 0x02
#define PLACETEXT_ABOVE 0x04
#define PLACETEXT_BELOW 0x08
#define PLACETEXT_IN    0x10
#define NG_HIGHLABEL    0x20

#define IDCMP_GADGETDOWN 0x20
#define IDCMP_GADGETUP   0x40

struct NewGadget {
	int16_t ng_LeftEdge, ng_TopEdge, ng_Width, ng_Height;
	APTR ng_GadgetText, ng_TextAttr;
	uint16_t ng_GadgetID;
	uint32_t ng_Flags;
	APTR ng_VisualInfo, ng_UserData;
} __attribute__((packed));

struct gtg {
	struct Gadget *g;
	int kind;
	char *label;                    /* without the underscore */
	int ul;                         /* index of the underlined char, -1 */
	uint32_t place;
	int disabled;
	/* values */
	int checked, active, number, min, max, level, top, total, visible;
	int vert, depth, coloroff, ncolors, readonly, showsel, selected;
	int arrows, border, pressed, dragging, dragoff;
	uint32_t titleplace;             /* MX: GTMX_TitlePlace, else no title */
	uint32_t levelfmt, dispfunc, levelplace;
	int maxlevellen;
	char **labels;                  /* cycle, mx */
	int nlabels;
	uint32_t list;                  /* listview: struct List * */
	char *text;
	int immediate, relverify;
	struct gtg *next;
};

static struct gtg *gts;

static struct gtg *find(struct Gadget *g)
{
	struct gtg *t;

	for (t = gts; t; t = t->next)
		if (t->g == g)
			return t;
	return NULL;
}

int gt_is_ours(struct Gadget *g)
{
	return find(g) != NULL;
}

static int fh(void) { return gui_topaz ? gui_topaz->tf_YSize : 8; }
static int fb(void) { return gui_topaz ? gui_topaz->tf_Baseline : 6; }
static int pen(int i) { return gui_pens[i]; }

static void text_at(struct RastPort *rp, int x, int y, const char *s, int n,
		    int fg, int ul)
{
	rp->FgPen = fg;
	rp->DrawMode = JAM1;
	rp->cp_x = x;
	rp->cp_y = y + fb();
	gfx_text(rp, s, n);
	if (ul >= 0 && ul < n) {
		int ux = x + gfx_textlength(rp, s, ul);
		int uw = gfx_textlength(rp, s + ul, 1);
		gfx_rectfill(rp, ux, y + fb() + 1, ux + uw - 1, y + fb() + 1, fg);
	}
}

static void label(struct awin *a, struct gtg *t)
{
	struct Gadget *g = t->g;
	struct RastPort *rp = a->rp;
	int n, w, x, y;
	uint32_t pl = t->place;

	if (!t->label || !*t->label)
		return;
	n = strlen(t->label);
	w = gfx_textlength(rp, t->label, n);
	if (!(pl & 0x1f))
		pl |= (t->kind == BUTTON_KIND) ? PLACETEXT_IN :
		      (t->kind == MX_KIND || t->kind == CHECKBOX_KIND) &&
		      0 ? PLACETEXT_RIGHT : PLACETEXT_LEFT;
	if (pl & PLACETEXT_IN) {
		x = g->LeftEdge + (g->Width - w) / 2;
		y = g->TopEdge + (g->Height - fh() + 1) / 2;
	} else if (pl & PLACETEXT_RIGHT) {
		x = g->LeftEdge + g->Width + 8;
		y = g->TopEdge + (g->Height - fh() + 1) / 2;
	} else if (pl & PLACETEXT_ABOVE) {
		x = g->LeftEdge + (g->Width - w) / 2;
		y = g->TopEdge - fh() - 2;
	} else if (pl & PLACETEXT_BELOW) {
		x = g->LeftEdge + (g->Width - w) / 2;
		y = g->TopEdge + g->Height + 2;
	} else {
		x = g->LeftEdge - w - 8;
		y = g->TopEdge + (g->Height - fh() + 1) / 2;
	}
	if (t->kind == MX_KIND && !(pl & ~PLACETEXT_LEFT & 0x1f))
		y = g->TopEdge - fh() - 2 > 0 ? y : y;  /* titles stay left */
	text_at(rp, x, y, t->label, n,
		(pl & NG_HIGHLABEL) ? pen(WBP_HIGHLIGHTTEXTPEN) :
		(t->pressed && (pl & PLACETEXT_IN)) ? pen(WBP_FILLTEXTPEN) :
		pen(WBP_TEXTPEN), t->ul);
}

static void ridge(struct RastPort *rp, int x, int y, int w, int h)
{
	gfx_bevel(rp, x, y, w, h, 0);
	gfx_bevel(rp, x + 1, y + 1, w - 2, h - 2, 1);
}

static void clear(struct RastPort *rp, int x, int y, int w, int h, int p)
{
	if (w > 0 && h > 0)
		gfx_rectfill(rp, x, y, x + w - 1, y + h - 1, p);
}

static void check_mark(struct RastPort *rp, int x, int y, int w, int h, int p)
{
	int cx = x + w / 2 - 4, cy = y + h / 2 - 2, i;

	for (i = 0; i < 2; i++) {
		gfx_rectfill(rp, cx + i, cy + 2 + i, cx + i + 1, cy + 2 + i, p);
		gfx_rectfill(rp, cx + 2 + i, cy + 4 - i + 0, cx + 3 + i, cy + 4 - i, p);
	}
	for (i = 0; i < 4; i++)
		gfx_rectfill(rp, cx + 4 + i, cy + 2 - i / 2 * 1 - (i > 1),
			     cx + 5 + i, cy + 2 - i / 2 - (i > 1), p);
}

/* the knob of a slider or scroller: [x,y,w,h] of the container */
static void knob_box(struct gtg *t, int x, int y, int w, int h, int *kx,
		     int *ky, int *kw, int *kh)
{
	int range, vis, pos, len = t->vert ? h - 4 : w - 4, kl;

	if (t->kind == SLIDER_KIND) {
		range = t->max - t->min + 1;
		vis = 1;
		pos = t->level - t->min;
	} else {
		range = t->total > 0 ? t->total : 1;
		vis = t->visible > range ? range : t->visible < 1 ? 1 : t->visible;
		pos = t->top;
	}
	if (range < 1)
		range = 1;
	kl = len * vis / range;
	if (t->kind == SLIDER_KIND)
		kl = len / (range < 8 ? range : 8) > 6 ? len / (range < 8 ? range : 8) : 6;
	if (kl < 6)
		kl = 6;
	if (kl > len)
		kl = len;
	if (range - vis > 0)
		pos = (len - kl) * pos / (range - vis);
	else
		pos = 0;
	if (pos < 0) pos = 0;
	if (pos > len - kl) pos = len - kl;
	if (t->vert) {
		*kx = x + 2, *kw = w - 4, *ky = y + 2 + pos, *kh = kl;
	} else {
		*ky = y + 2, *kh = h - 4, *kx = x + 2 + pos, *kw = kl;
	}
}

/* the scroller's own area without its arrows */
static void scroll_area(struct gtg *t, int *x, int *y, int *w, int *h)
{
	struct Gadget *g = t->g;

	*x = g->LeftEdge, *y = g->TopEdge, *w = g->Width, *h = g->Height;
	if (t->kind == SCROLLER_KIND && t->arrows) {
		if (t->vert)
			*h -= 2 * t->arrows;
		else
			*w -= 2 * t->arrows;
	}
}

static void arrow(struct RastPort *rp, int x, int y, int w, int h, int dir,
		  int pressed)
{
	int cx = x + w / 2, cy = y + h / 2, i;

	clear(rp, x, y, w, h, pressed ? pen(WBP_FILLPEN) : pen(WBP_BACKGROUNDPEN));
	gfx_bevel(rp, x, y, w, h, pressed);
	for (i = 0; i < 3; i++) {
		switch (dir) {
		case 0: gfx_rectfill(rp, cx - i, cy - 1 + i, cx + i, cy - 1 + i, pen(WBP_TEXTPEN)); break;
		case 1: gfx_rectfill(rp, cx - i, cy + 1 - i, cx + i, cy + 1 - i, pen(WBP_TEXTPEN)); break;
		case 2: gfx_rectfill(rp, cx - 1 + i, cy - i, cx - 1 + i, cy + i, pen(WBP_TEXTPEN)); break;
		case 3: gfx_rectfill(rp, cx + 1 - i, cy - i, cx + 1 - i, cy + i, pen(WBP_TEXTPEN)); break;
		}
	}
}

static int list_count(uint32_t list)
{
	struct Node *n;
	int c = 0;

	if (!list || list == 0xffffffff)
		return 0;
	for (n = PTR(((struct List *)PTR(list))->lh_Head); n->ln_Succ;
	     n = PTR(n->ln_Succ))
		c++;
	return c;
}

static struct Node *list_nth(uint32_t list, int i)
{
	struct Node *n;

	if (!list || list == 0xffffffff)
		return NULL;
	for (n = PTR(((struct List *)PTR(list))->lh_Head); n->ln_Succ;
	     n = PTR(n->ln_Succ))
		if (!i--)
			return n;
	return NULL;
}

#define LV_SCROLLW 16

static int lv_rows(struct gtg *t)
{
	int h = t->g->Height - 4 - (t->showsel ? fh() + 4 : 0);
	return h / fh() > 0 ? h / fh() : 1;
}

struct lvbuf {
	char s[64];
	int n;
};

static void put_lv(int c, void *ctx)
{
	struct lvbuf *b = ctx;
	if (b->n < 63)
		b->s[b->n++] = c;
	b->s[b->n] = 0;
}

/* GTSL_LevelFormat (RawDoFmt, one LONG) of the level, or of what
   GTSL_DispFunc (a C function: LONG f(struct Gadget *, WORD)) makes of it */
static void slider_level(struct awin *a, struct gtg *t)
{
	extern struct frame *resume_frame;
	struct RastPort *rp = a->rp;
	struct Gadget *g = t->g;
	struct lvbuf b = { "", 0 };
	int32_t *v = amem_alloc(4);
	int cw = gui_topaz ? gui_topaz->tf_XSize : 8, len, w, x, y;

	*v = t->level;
	if (t->dispfunc && resume_frame) {
		struct regs r;
		uint32_t *sp = (uint32_t *)((uint8_t *)resume_frame - 256);
		memset(&r, 0, sizeof(r));
		sp[0] = APTR_OF(g);
		sp[1] = (uint32_t)(int32_t)(int16_t)t->level;
		r.a[7] = APTR_OF(sp);
		r.a[6] = APTR_OF(SysBase);
		*v = call_amiga(&r, t->dispfunc);
	}
	fmt_do(PTR(t->levelfmt), APTR_OF(v), put_lv, &b, 0);
	amem_free(v);
	len = t->maxlevellen ? t->maxlevellen : b.n;
	w = len * cw;
	y = g->TopEdge + (g->Height - fh() + 1) / 2;
	if (t->levelplace & PLACETEXT_RIGHT)
		x = g->LeftEdge + g->Width + 8;
	else
		x = g->LeftEdge - w - 8;
	clear(rp, x, y, w, fh(), pen(WBP_BACKGROUNDPEN));
	text_at(rp, x + w - b.n * cw, y, b.s, b.n, pen(WBP_TEXTPEN), -1);
}

int gt_render(struct awin *a, struct Gadget *g)
{
	struct gtg *t = find(g);
	struct RastPort *rp = a->rp;
	int x = g->LeftEdge, y = g->TopEdge, w = g->Width, h = g->Height, i;

	if (!t)
		return 0;
	switch (t->kind) {
	case BUTTON_KIND:
		clear(rp, x, y, w, h, t->pressed ? pen(WBP_FILLPEN) :
		      pen(WBP_BACKGROUNDPEN));
		gfx_bevel(rp, x, y, w, h, t->pressed);
		label(a, t);
		break;
	case CHECKBOX_KIND:
		clear(rp, x, y, w, h, pen(WBP_BACKGROUNDPEN));
		gfx_bevel(rp, x, y, w, h, t->pressed);
		if (t->checked)
			check_mark(rp, x, y, w, h, pen(WBP_TEXTPEN));
		label(a, t);
		break;
	case CYCLE_KIND: {
		const char *s = t->active < t->nlabels ? t->labels[t->active] : "";
		int n = strlen(s), tw = gfx_textlength(rp, s, n);
		clear(rp, x, y, w, h, t->pressed ? pen(WBP_FILLPEN) :
		      pen(WBP_BACKGROUNDPEN));
		gfx_bevel(rp, x, y, w, h, t->pressed);
		/* the cycle glyph and its separator */
		gfx_rectfill(rp, x + 6, y + 3, x + 6, y + h - 4, pen(WBP_TEXTPEN));
		gfx_rectfill(rp, x + 6, y + 3, x + 12, y + 3, pen(WBP_TEXTPEN));
		gfx_rectfill(rp, x + 6, y + h - 4, x + 12, y + h - 4, pen(WBP_TEXTPEN));
		gfx_rectfill(rp, x + 11, y + 4, x + 13, y + 4, pen(WBP_TEXTPEN));
		gfx_rectfill(rp, x + 19, y + 2, x + 19, y + h - 3, pen(WBP_SHADOWPEN));
		gfx_rectfill(rp, x + 20, y + 2, x + 20, y + h - 3, pen(WBP_SHINEPEN));
		text_at(rp, x + 20 + (w - 20 - tw) / 2, y + (h - fh() + 1) / 2, s, n,
			t->pressed ? pen(WBP_FILLTEXTPEN) : pen(WBP_TEXTPEN), -1);
		label(a, t);
		break;
	}
	case MX_KIND: {
		int bh = fh() + 1 > 9 ? fh() + 1 : 9, sp = 1;
		for (i = 0; i < t->nlabels; i++) {
			int by = y + i * (bh + sp);
			clear(rp, x, by, 17, 9, pen(WBP_BACKGROUNDPEN));
			gfx_bevel(rp, x, by, 17, 9, i == t->active);
			if (i == t->active)
				clear(rp, x + 3, by + 2, 11, 5, pen(WBP_FILLPEN));
			text_at(rp, x + 17 + 8, by + (9 - fh() + 1) / 2,
				t->labels[i], strlen(t->labels[i]), pen(WBP_TEXTPEN), -1);
		}
		/* V37 ignores an MX gadget's text; V39 places it as a title
		   only when asked (GTMX_TitlePlace) */
		if (t->label && *t->label && t->titleplace) {
			uint32_t keep = t->place;
			t->place = t->titleplace;
			label(a, t);
			t->place = keep;
		}
		break;
	}
	case STRING_KIND:
	case INTEGER_KIND:
		ridge(rp, x - 6, y - 3, w + 12, h + 6);
		label(a, t);
		return 0;               /* Intuition draws the text and cursor */
	case TEXT_KIND:
	case NUMBER_KIND: {
		char nb[16];
		const char *s = t->text ? t->text : "";
		clear(rp, x, y, w, h, pen(WBP_BACKGROUNDPEN));
		if (t->border)
			gfx_bevel(rp, x, y, w, h, 1);
		if (t->kind == NUMBER_KIND) {
			snprintf(nb, sizeof(nb), "%d", t->number);
			s = nb;
		}
		text_at(rp, x + (t->border ? 6 : 0), y + (h - fh() + 1) / 2, s,
			strlen(s), pen(WBP_TEXTPEN), -1);
		label(a, t);
		break;
	}
	case SLIDER_KIND:
	case SCROLLER_KIND: {
		int sx, sy, sw, sh, kx, ky, kw, kh;
		scroll_area(t, &sx, &sy, &sw, &sh);
		clear(rp, sx, sy, sw, sh, pen(WBP_BACKGROUNDPEN));
		if (t->kind == SLIDER_KIND)
			gfx_bevel(rp, sx, sy, sw, sh, 1);
		else
			gfx_bevel(rp, sx, sy, sw, sh, 0);
		knob_box(t, sx, sy, sw, sh, &kx, &ky, &kw, &kh);
		clear(rp, kx, ky, kw, kh, pen(WBP_FILLPEN));
		gfx_bevel(rp, kx, ky, kw, kh, 0);
		if (t->kind == SLIDER_KIND && t->levelfmt)
			slider_level(a, t);
		if (t->kind == SCROLLER_KIND && t->arrows) {
			if (t->vert) {
				arrow(rp, x, y + h - 2 * t->arrows, w, t->arrows, 0, 0);
				arrow(rp, x, y + h - t->arrows, w, t->arrows, 1, 0);
			} else {
				arrow(rp, x + w - 2 * t->arrows, y, t->arrows, h, 2, 0);
				arrow(rp, x + w - t->arrows, y, t->arrows, h, 3, 0);
			}
		}
		label(a, t);
		break;
	}
	case LISTVIEW_KIND: {
		int lw = w - LV_SCROLLW, rows = lv_rows(t), n = list_count(t->list);
		int ly = y;
		clear(rp, x, y, lw, h - (t->showsel ? fh() + 4 : 0),
		      pen(WBP_BACKGROUNDPEN));
		gfx_bevel(rp, x, y, lw, rows * fh() + 4, 1);
		for (i = 0; i < rows; i++) {
			struct Node *nd = list_nth(t->list, t->top + i);
			const char *s;
			int sel;
			if (!nd)
				break;
			s = nd->ln_Name ? PTR(nd->ln_Name) : "";
			sel = t->top + i == t->selected && !t->readonly;
			if (sel)
				clear(rp, x + 2, ly + 2 + i * fh(), lw - 4, fh(),
				      pen(WBP_FILLPEN));
			rp->cp_x = 0;
			{
				int len = strlen(s), max = (lw - 8) / (gui_topaz ? gui_topaz->tf_XSize : 8);
				text_at(rp, x + 4, ly + 2 + i * fh(), s,
					len < max ? len : max,
					sel ? pen(WBP_FILLTEXTPEN) : pen(WBP_TEXTPEN), -1);
			}
		}
		/* its scroller */
		{
			struct gtg sc = *t;
			int ah = 8;
			sc.kind = SCROLLER_KIND;
			sc.total = n;
			sc.visible = rows;
			sc.vert = 1;
			clear(rp, x + lw, y, LV_SCROLLW, rows * fh() + 4 - 2 * ah,
			      pen(WBP_BACKGROUNDPEN));
			gfx_bevel(rp, x + lw, y, LV_SCROLLW, rows * fh() + 4 - 2 * ah, 0);
			{
				int kx, ky, kw, kh;
				knob_box(&sc, x + lw, y, LV_SCROLLW,
					 rows * fh() + 4 - 2 * ah, &kx, &ky, &kw, &kh);
				clear(rp, kx, ky, kw, kh, pen(WBP_FILLPEN));
				gfx_bevel(rp, kx, ky, kw, kh, 0);
			}
			arrow(rp, x + lw, y + rows * fh() + 4 - 2 * ah, LV_SCROLLW, ah, 0, 0);
			arrow(rp, x + lw, y + rows * fh() + 4 - ah, LV_SCROLLW, ah, 1, 0);
		}
		if (t->showsel) {
			struct Node *nd = list_nth(t->list, t->selected);
			int sy = y + rows * fh() + 4;
			clear(rp, x, sy, w, fh() + 4, pen(WBP_BACKGROUNDPEN));
			gfx_bevel(rp, x, sy, w, fh() + 4, 1);
			if (nd && nd->ln_Name)
				text_at(rp, x + 4, sy + 2, PTR(nd->ln_Name),
					strlen(PTR(nd->ln_Name)), pen(WBP_TEXTPEN), -1);
		}
		if (!(t->place & 0x1f))
			t->place |= PLACETEXT_ABOVE;
		label(a, t);
		break;
	}
	case PALETTE_KIND: {
		int nc = t->ncolors ? t->ncolors : 1 << (t->depth ? t->depth : 1);
		int cols = nc > 8 ? 8 : nc, rows = (nc + cols - 1) / cols;
		int cw = (w - 2) / cols, ch = (h - 2) / rows;
		clear(rp, x, y, w, h, pen(WBP_BACKGROUNDPEN));
		gfx_bevel(rp, x, y, w, h, 1);
		for (i = 0; i < nc; i++) {
			int cx = x + 1 + (i % cols) * cw, cy = y + 1 + (i / cols) * ch;
			clear(rp, cx + 1, cy + 1, cw - 2, ch - 2, t->coloroff + i);
			if (t->coloroff + i == t->active)
				gfx_bevel(rp, cx, cy, cw, ch, 1);
		}
		label(a, t);
		break;
	}
	default:
		return 1;               /* GENERIC: the program's own */
	}
	if (t->disabled) {
		uint16_t pat[2] = { 0x8888, 0x2222 };
		uint16_t *p = amem_alloc(4);
		APTR ap = rp->AreaPtrn;
		int8_t sz = rp->AreaPtSz;
		memcpy(p, pat, 4);
		rp->AreaPtrn = APTR_OF(p);
		rp->AreaPtSz = 1;
		rp->FgPen = pen(WBP_BLOCKPEN);
		rp->DrawMode = JAM1;
		gfx_rectfill(rp, x, y, x + w - 1, y + h - 1, -1);
		rp->AreaPtrn = ap;
		rp->AreaPtSz = sz;
		amem_free(p);
	}
	return 1;
}

static void redraw(struct awin *a, struct gtg *t)
{
	gt_render(a, t->g);
}

extern void intui_put_imsg(struct awin *a, uint32_t class, uint16_t code,
			   uint16_t qual, APTR iaddr, int mx, int my);

static int slider_value(struct gtg *t, int mx, int my)
{
	int sx, sy, sw, sh, kx, ky, kw, kh, len, pos, range, vis;

	scroll_area(t, &sx, &sy, &sw, &sh);
	knob_box(t, sx, sy, sw, sh, &kx, &ky, &kw, &kh);
	len = (t->vert ? sh : sw) - 4 - (t->vert ? kh : kw);
	pos = (t->vert ? my - sy - 2 : mx - sx - 2) - t->dragoff;
	if (t->kind == SLIDER_KIND) {
		range = t->max - t->min;
		if (len <= 0)
			return t->min;
		pos = t->min + (pos * range + len / 2) / len;
		return pos < t->min ? t->min : pos > t->max ? t->max : pos;
	}
	vis = t->visible < 1 ? 1 : t->visible;
	range = t->total - vis;
	if (range <= 0 || len <= 0)
		return 0;
	pos = (pos * range + len / 2) / len;
	return pos < 0 ? 0 : pos > range ? range : pos;
}

int gt_handle_down(struct awin *a, struct Gadget *g, int mx, int my)
{
	struct gtg *t = find(g);
	int i;

	if (!t || t->disabled)
		return t != NULL;
	switch (t->kind) {
	case STRING_KIND:
	case INTEGER_KIND:
	case GENERIC_KIND:
		return 0;
	case BUTTON_KIND:
	case CHECKBOX_KIND:
	case CYCLE_KIND:
		t->pressed = 1;
		redraw(a, t);
		if (t->kind == BUTTON_KIND && t->immediate)
			intui_put_imsg(a, IDCMP_GADGETDOWN, 0, 0, APTR_OF(g), mx, my);
		return 1;
	case MX_KIND: {
		int bh = fh() + 1 > 9 ? fh() + 1 : 9;
		i = (my - g->TopEdge) / (bh + 1);
		if (i >= 0 && i < t->nlabels && i != t->active) {
			t->active = i;
			redraw(a, t);
			intui_put_imsg(a, IDCMP_GADGETDOWN, i, 0, APTR_OF(g), mx, my);
		}
		a->agad = NULL;
		return 1;
	}
	case PALETTE_KIND: {
		int nc = t->ncolors ? t->ncolors : 1 << (t->depth ? t->depth : 1);
		int cols = nc > 8 ? 8 : nc, rows = (nc + cols - 1) / cols;
		int cw = (g->Width - 2) / cols, ch = (g->Height - 2) / rows;
		int c = (mx - g->LeftEdge - 1) / cw + ((my - g->TopEdge - 1) / ch) * cols;
		if (c >= 0 && c < nc) {
			t->active = t->coloroff + c;
			redraw(a, t);
			intui_put_imsg(a, IDCMP_GADGETUP, t->active, 0, APTR_OF(g),
				       mx, my);
		}
		a->agad = NULL;
		return 1;
	}
	case SLIDER_KIND:
	case SCROLLER_KIND: {
		int sx, sy, sw, sh, kx, ky, kw, kh, v;
		scroll_area(t, &sx, &sy, &sw, &sh);
		if (t->kind == SCROLLER_KIND && t->arrows &&
		    (t->vert ? my >= sy + sh : mx >= sx + sw)) {
			int up = t->vert ? my < sy + sh + t->arrows :
					   mx < sx + sw + t->arrows;
			int max = t->total - t->visible;
			t->top += up ? -1 : 1;
			if (t->top > max) t->top = max;
			if (t->top < 0) t->top = 0;
			redraw(a, t);
			intui_put_imsg(a, IDCMP_GADGETUP, t->top, 0, APTR_OF(g), mx, my);
			a->agad = NULL;
			return 1;
		}
		knob_box(t, sx, sy, sw, sh, &kx, &ky, &kw, &kh);
		if (mx >= kx && mx < kx + kw && my >= ky && my < ky + kh) {
			t->dragging = 1;
			t->dragoff = t->vert ? my - ky : mx - kx;
			return 1;
		}
		/* page toward the click */
		v = t->kind == SLIDER_KIND ? t->level : t->top;
		if (t->vert ? my < ky : mx < kx)
			v -= t->kind == SLIDER_KIND ? 1 : (t->visible > 1 ? t->visible - 1 : 1);
		else
			v += t->kind == SLIDER_KIND ? 1 : (t->visible > 1 ? t->visible - 1 : 1);
		if (t->kind == SLIDER_KIND)
			t->level = v < t->min ? t->min : v > t->max ? t->max : v;
		else {
			int max = t->total - t->visible;
			t->top = v > max ? max : v;
			if (t->top < 0) t->top = 0;
		}
		redraw(a, t);
		intui_put_imsg(a, t->immediate ? IDCMP_GADGETDOWN : IDCMP_GADGETUP,
			       t->kind == SLIDER_KIND ? t->level : t->top, 0,
			       APTR_OF(g), mx, my);
		a->agad = NULL;
		return 1;
	}
	case LISTVIEW_KIND: {
		int lw = g->Width - LV_SCROLLW, rows = lv_rows(t), ah = 8;
		int n = list_count(t->list);
		if (mx >= g->LeftEdge + lw) {
			int ab = g->TopEdge + rows * fh() + 4 - 2 * ah;
			if (my >= ab) {
				t->top += my < ab + ah ? -1 : 1;
			} else {
				int y = my - g->TopEdge;
				t->top = n > rows ? (n - rows) * y / (rows * fh() + 4 - 2 * ah) : 0;
			}
			if (t->top > n - rows) t->top = n - rows;
			if (t->top < 0) t->top = 0;
			redraw(a, t);
			a->agad = NULL;
			return 1;
		}
		i = (my - g->TopEdge - 2) / fh();
		if (!t->readonly && i >= 0 && i < rows && t->top + i < n) {
			t->selected = t->top + i;
			redraw(a, t);
			intui_put_imsg(a, IDCMP_GADGETUP, t->selected, 0, APTR_OF(g),
				       mx, my);
		}
		a->agad = NULL;
		return 1;
	}
	default:
		return 1;
	}
}

int gt_handle_move(struct awin *a, struct Gadget *g, int mx, int my)
{
	struct gtg *t = find(g);
	int in, v;

	if (!t)
		return 0;
	if (t->kind == SLIDER_KIND || t->kind == SCROLLER_KIND) {
		if (!t->dragging)
			return 1;
		v = slider_value(t, mx, my);
		if (v != (t->kind == SLIDER_KIND ? t->level : t->top)) {
			if (t->kind == SLIDER_KIND)
				t->level = v;
			else
				t->top = v;
			redraw(a, t);
			intui_put_imsg(a, 0x10 /* MOUSEMOVE */, v, 0, APTR_OF(g), mx, my);
		}
		return 1;
	}
	if (t->kind == BUTTON_KIND || t->kind == CHECKBOX_KIND ||
	    t->kind == CYCLE_KIND) {
		in = mx >= g->LeftEdge && my >= g->TopEdge &&
		     mx < g->LeftEdge + g->Width && my < g->TopEdge + g->Height;
		if (in != t->pressed) {
			t->pressed = in;
			redraw(a, t);
		}
		return 1;
	}
	return t->kind != STRING_KIND && t->kind != INTEGER_KIND &&
	       t->kind != GENERIC_KIND;
}

int gt_handle_up(struct awin *a, struct Gadget *g, int mx, int my, int inside)
{
	struct gtg *t = find(g);

	if (!t)
		return 0;
	switch (t->kind) {
	case BUTTON_KIND:
		t->pressed = 0;
		redraw(a, t);
		if (inside)
			intui_put_imsg(a, IDCMP_GADGETUP, 0, 0, APTR_OF(g), mx, my);
		return 1;
	case CHECKBOX_KIND:
		t->pressed = 0;
		if (inside) {
			t->checked = !t->checked;
			if (t->checked)
				g->Flags |= 0x80;
			else
				g->Flags &= ~0x80;
		}
		redraw(a, t);
		if (inside)
			intui_put_imsg(a, IDCMP_GADGETUP, t->checked, 0, APTR_OF(g),
				       mx, my);
		return 1;
	case CYCLE_KIND:
		t->pressed = 0;
		if (inside && t->nlabels)
			t->active = (t->active + 1) % t->nlabels;
		redraw(a, t);
		if (inside)
			intui_put_imsg(a, IDCMP_GADGETUP, t->active, 0, APTR_OF(g),
				       mx, my);
		return 1;
	case SLIDER_KIND:
	case SCROLLER_KIND:
		if (t->dragging) {
			t->dragging = 0;
			intui_put_imsg(a, IDCMP_GADGETUP,
				       t->kind == SLIDER_KIND ? t->level : t->top,
				       0, APTR_OF(g), mx, my);
		}
		return 1;
	case STRING_KIND:
	case INTEGER_KIND:
	case GENERIC_KIND:
		return 0;
	default:
		return 1;
	}
}

int gt_key(struct awin *a, struct Gadget *g, int ch, int raw, int qual)
{
	(void)a; (void)g; (void)ch; (void)raw; (void)qual;
	return 0;               /* keyboard shortcuts are the program's */
}

/* ---- creating gadgets ---- */

static char *dup_label(const char *s, int us, int *ul)
{
	char *d;
	int i, j;

	*ul = -1;
	if (!s)
		return NULL;
	d = malloc(strlen(s) + 1);
	for (i = j = 0; s[i]; i++) {
		if (us && s[i] == us && s[i + 1] && *ul < 0) {
			*ul = j;
			continue;
		}
		d[j++] = s[i];
	}
	d[j] = 0;
	return d;
}

static void set_labels(struct gtg *t, uint32_t arr)
{
	uint32_t *p = PTR(arr);
	int n = 0;

	free(t->labels);
	t->labels = NULL;
	t->nlabels = 0;
	if (!arr)
		return;
	while (p[n])
		n++;
	t->labels = malloc((n + 1) * sizeof(char *));
	for (int i = 0; i < n; i++)
		t->labels[i] = PTR(p[i]);
	t->nlabels = n;
}

static void apply_tags(struct gtg *t, struct TagItem *tags, int create)
{
	extern struct TagItem *util_next_tag(struct TagItem **tp);
	struct Gadget *g = t->g;
	struct TagItem *x;

	while ((x = util_next_tag(&tags))) {
		uint32_t d = x->ti_Data;
		switch (x->ti_Tag) {
		case GA_Disabled:
			t->disabled = d != 0;
			if (d)
				g->Flags |= 0x100;
			else
				g->Flags &= ~0x100;
			break;
		case GA_Immediate: t->immediate = d != 0; break;
		case GA_RelVerify: t->relverify = d != 0; break;
		case GTCB_Checked:
			t->checked = d != 0;
			if (d)
				g->Flags |= 0x80;
			else
				g->Flags &= ~0x80;
			break;
		case GTCY_Labels: set_labels(t, d); break;
		case GTCY_Active: t->active = d; break;
		case GTMX_Labels: set_labels(t, d); break;
		case GTMX_Active: t->active = d; break;
		case GTTX_Text:
			free(t->text);
			t->text = d ? strdup(PTR(d)) : NULL;
			break;
		case GTTX_Border: case GTNM_Border: t->border = d != 0; break;
		case GTNM_Number: t->number = d; break;
		case GTSL_Min: t->min = (int16_t)d; break;
		case GTSL_Max: t->max = (int16_t)d; break;
		case GTSL_Level: t->level = (int16_t)d; break;
		case GTSC_Top: t->top = d; break;
		case GTSC_Total: t->total = d; break;
		case GTSC_Visible: t->visible = d; break;
		case GTSC_Arrows: t->arrows = d; break;
		case GTMX_TitlePlace: t->titleplace = d; break;
		case GTSL_MaxLevelLen: t->maxlevellen = d; break;
		case GTSL_LevelFormat: t->levelfmt = d; break;
		case GTSL_LevelPlace: t->levelplace = d; break;
		case GTSL_DispFunc: t->dispfunc = d; break;
		case PGA_Freedom: t->vert = d == 2; break;
		case GTLV_Labels: t->list = d; break;
		case GTLV_Top: t->top = d; break;
		case GTLV_MakeVisible: t->top = d; break;
		case GTLV_ReadOnly: t->readonly = d != 0; break;
		case GTLV_ShowSelected: t->showsel = 1; break;
		case GTLV_Selected: t->selected = (int32_t)d; break;
		case GTPA_Depth: t->depth = d; break;
		case GTPA_Color: t->active = d; break;
		case GTPA_ColorOffset: t->coloroff = d; break;
		case GTPA_NumColors: t->ncolors = d; break;
		case GTST_String:
		case GTIN_Number:
			if (g->SpecialInfo) {
				struct StringInfo *si = PTR(g->SpecialInfo);
				char *buf = PTR(si->Buffer);
				if (x->ti_Tag == GTST_String)
					snprintf(buf, si->MaxChars, "%s",
						 d ? (char *)PTR(d) : "");
				else {
					snprintf(buf, si->MaxChars, "%d", (int)d);
					si->LongInt = d;
				}
				si->BufferPos = strlen(buf);
				si->DispPos = 0;
			}
			break;
		}
	}
	(void)create;
}

static uint32_t tag_value(struct TagItem *tags, uint32_t tag, uint32_t def)
{
	extern struct TagItem *util_next_tag(struct TagItem **tp);
	struct TagItem *x;

	while ((x = util_next_tag(&tags)))
		if (x->ti_Tag == tag)
			return x->ti_Data;
	return def;
}

static void gt_CreateContext(struct frame *f)
{
	struct Gadget *g = amem_alloc(sizeof(*g));

	g->GadgetType = 1;
	g->Flags = 0x0003;                      /* GADGHNONE, 0x0 size */
	*(uint32_t *)PTR(A(0)) = APTR_OF(g);
	RET(g);
}

static void gt_CreateGadgetA(struct frame *f)
{
	int kind = D(0);
	struct Gadget *prev = PTR(A(0)), *g;
	struct NewGadget *ng = PTR(A(1));
	struct TagItem *tags = PTR(A(2));
	struct gtg *t;
	int us;

	if (!prev || !ng) {
		RET(0);
		return;
	}
	g = amem_alloc(sizeof(*g) + 8);
	t = calloc(1, sizeof(*t));
	t->g = g;
	t->kind = kind;
	t->place = ng->ng_Flags;
	t->max = 15;
	t->visible = 2;
	t->selected = -1;
	us = tag_value(tags, GT_Underscore, 0) & 0xff;
	t->label = dup_label(ng->ng_GadgetText ? PTR(ng->ng_GadgetText) : NULL,
			     us, &t->ul);
	g->LeftEdge = ng->ng_LeftEdge;
	g->TopEdge = ng->ng_TopEdge;
	g->Width = ng->ng_Width;
	g->Height = ng->ng_Height;
	g->GadgetID = ng->ng_GadgetID;
	g->UserData = ng->ng_UserData;
	g->GadgetType = 1 | 0x0100;             /* BOOLGADGET | GTYP_GADTOOLS */
	g->Flags = 0x0003;                      /* GADGHNONE: we draw */
	g->Activation = 0x0001;                 /* RELVERIFY */
	if (kind == CHECKBOX_KIND) {
		if (!tag_value(tags, GT(68), 0)) {      /* GTCB_Scaled */
			g->Width = 26;
			g->Height = 11;
		}
	}
	if (kind == STRING_KIND || kind == INTEGER_KIND) {
		struct StringInfo *si = amem_alloc(sizeof(*si));
		int max = tag_value(tags, kind == STRING_KIND ? GTST_MaxChars :
				    GTIN_MaxChars, kind == STRING_KIND ? 64 : 10) + 1;
		si->Buffer = APTR_OF(amem_alloc(max + 1));
		si->UndoBuffer = APTR_OF(amem_alloc(max + 1));
		si->MaxChars = max;
		g->SpecialInfo = APTR_OF(si);
		g->GadgetType = 4 | 0x0100;     /* STRGADGET */
		if (kind == INTEGER_KIND) {
			g->Activation |= 0x0800;        /* LONGINT */
			snprintf(PTR(si->Buffer), max, "0");
		}
		/* the text area sits inside the ridge */
		g->LeftEdge += 6;
		g->TopEdge += 3;
		g->Width -= 12;
		g->Height -= 6;
	}
	if (kind == SLIDER_KIND)
		t->max = 15, t->min = 0;
	if (kind == SCROLLER_KIND)
		t->total = 1, t->visible = 1;
	apply_tags(t, tags, 1);
	if (kind == SLIDER_KIND || kind == SCROLLER_KIND)
		g->Activation |= 0x0008;        /* FOLLOWMOUSE */
	t->next = gts;
	gts = t;
	prev->NextGadget = APTR_OF(g);
	RET(g);
}

static void gt_FreeGadgets(struct frame *f)
{
	struct Gadget *g = PTR(A(0)), *n;
	struct gtg **pp, *t;

	for (; g; g = n) {
		n = PTR(g->NextGadget);
		for (pp = &gts; (t = *pp); pp = &t->next)
			if (t->g == g) {
				*pp = t->next;
				free(t->label);
				free(t->labels);
				free(t->text);
				free(t);
				break;
			}
	}
}

static void gt_SetGadgetAttrsA(struct frame *f)
{
	struct gtg *t = find(PTR(A(0)));
	struct awin *a = A(1) ? gui_by_window(A(1)) : NULL;

	if (!t)
		return;
	apply_tags(t, PTR(A(3)), 0);
	if (a)
		intui_render_gadgets(a, t->g, 1);
}

static void gt_GetGadgetAttrsA(struct frame *f)
{
	extern struct TagItem *util_next_tag(struct TagItem **tp);
	struct gtg *t = find(PTR(A(0)));
	struct TagItem *tags = PTR(A(3)), *x;
	int n = 0;

	RET(0);
	if (!t)
		return;
	while ((x = util_next_tag(&tags))) {
		uint32_t *d = PTR(x->ti_Data), v;
		switch (x->ti_Tag) {
		case GA_Disabled: v = t->disabled; break;
		case GTCB_Checked: v = t->checked; break;
		case GTCY_Active: case GTMX_Active: case GTPA_Color: v = t->active; break;
		case GTSL_Level: v = t->level; break;
		case GTSL_Min: v = t->min; break;
		case GTSL_Max: v = t->max; break;
		case GTSC_Top: case GTLV_Top: v = t->top; break;
		case GTSC_Total: v = t->total; break;
		case GTSC_Visible: v = t->visible; break;
		case GTLV_Selected: v = t->selected; break;
		case GTLV_Labels: v = t->list; break;
		case GTNM_Number: v = t->number; break;
		case GTST_String: {
			struct StringInfo *si = PTR(t->g->SpecialInfo);
			v = si ? si->Buffer : 0;
			break;
		}
		case GTIN_Number: {
			struct StringInfo *si = PTR(t->g->SpecialInfo);
			v = si ? strtol(PTR(si->Buffer), NULL, 10) : 0;
			break;
		}
		default:
			continue;
		}
		*d = v;
		n++;
	}
	RET(n);
}

static void gt_RefreshWindow(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));

	if (a)
		intui_render_gadgets(a, PTR(a->w->FirstGadget), -1);
}

static void gt_GetIMsg(struct frame *f)
{
	extern struct Node *exec_getmsg(struct MsgPort *p);
	RET(exec_getmsg(PTR(A(0))));
}

static void gt_ReplyIMsg(struct frame *f)
{
	extern void exec_reply(struct Message *m);
	if (A(1))
		exec_reply(PTR(A(1)));
}

static void gt_Filter(struct frame *f) { RET(A(1)); }
static void gt_Nop(struct frame *f) { (void)f; }

static void gt_DrawBevelBoxA(struct frame *f)
{
	struct RastPort *rp = PTR(A(0));
	struct TagItem *tags = PTR(A(1));
	int x = (int16_t)D(0), y = (int16_t)D(1), w = (int16_t)D(2), h = (int16_t)D(3);
	int rec = tag_value(tags, GTBB_Recessed, 0) != 0;

	if (tag_value(tags, GTBB_FrameType, 1) == 2)    /* BBFT_RIDGE */
		ridge(rp, x, y, w, h);
	else
		gfx_bevel(rp, x, y, w, h, rec);
}

static void gt_GetVisualInfoA(struct frame *f)
{
	RET(gui_connect() ? 0 : APTR_OF(amem_alloc(16)));
}

static void gt_FreeVisualInfo(struct frame *f)
{
	if (A(0))
		amem_free(PTR(A(0)));
}

/* ---- menus: NewMenu arrays into Menu/MenuItem strips ---- */

struct NewMenu {
	uint8_t nm_Type, nm_Pad;
	APTR nm_Label, nm_CommKey;
	uint16_t nm_Flags;
	int32_t nm_MutualExclude;
	APTR nm_UserData;
};

static struct IntuiText *mk_itext(const char *s)
{
	struct IntuiText *it = amem_alloc(sizeof(*it));
	it->FrontPen = gui_pens[WBP_BARDETAILPEN];
	it->DrawMode = JAM1;
	it->LeftEdge = 2;
	it->TopEdge = 1;
	it->IText = APTR_OF(s);
	return it;
}

static void gt_CreateMenusA(struct frame *f)
{
	struct NewMenu *nm = PTR(A(0));
	struct Menu *first = NULL, *menu = NULL;
	struct MenuItem *item = NULL, *sub = NULL;

	for (; nm->nm_Type; nm++) {
		int type = nm->nm_Type & 0x7f;
		if (nm->nm_Type & 0x40)                 /* NM_IGNORE */
			continue;
		if (type == 1) {                        /* NM_TITLE */
			struct Menu *m = amem_alloc(sizeof(*m) + 4);
			m->MenuName = nm->nm_Label;
			m->Flags = (nm->nm_Flags & 1) ? 0 : 0x0001;
			*(uint32_t *)((uint8_t *)m + sizeof(*m)) = nm->nm_UserData;
			if (menu)
				menu->NextMenu = APTR_OF(m);
			else
				first = m;
			menu = m;
			item = sub = NULL;
		} else if ((type == 2 || type == 3) && menu) {
			struct MenuItem *it = amem_alloc(sizeof(*it) + 4);
			int bar = nm->nm_Label == 0xffffffff;
			it->Flags = (nm->nm_Flags & 0x10 ? 0 : 0x0010) |
				    (nm->nm_Flags & (0x0001 | 0x0100 | 0x0008));
			if (bar) {
				struct Image *im = amem_alloc(sizeof(*im));
				it->ItemFill = APTR_OF(im);
			} else if (nm->nm_Type & 0x80) {        /* IM_ITEM: an Image */
				it->ItemFill = nm->nm_Label;
			} else {
				it->Flags |= 0x0002;            /* ITEMTEXT */
				it->ItemFill = APTR_OF(mk_itext(PTR(nm->nm_Label)));
			}
			if (nm->nm_CommKey && !bar) {
				it->Command = *(char *)PTR(nm->nm_CommKey);
				it->Flags |= 0x0004;            /* COMMSEQ */
			}
			it->MutualExclude = nm->nm_MutualExclude;
			it->NextSelect = 0xffff;
			*(uint32_t *)((uint8_t *)it + sizeof(*it)) = nm->nm_UserData;
			if (type == 2) {
				if (item)
					item->NextItem = APTR_OF(it);
				else
					menu->FirstItem = APTR_OF(it);
				item = it;
				sub = NULL;
			} else if (item) {
				if (sub)
					sub->NextItem = APTR_OF(it);
				else
					item->SubItem = APTR_OF(it);
				sub = it;
			}
		}
	}
	RET(first);
}

static void gt_FreeMenus(struct frame *f) { (void)f; }
static void gt_True(struct frame *f) { RET(1); }

/* the vector table: see the header comment about where it comes from */
static const struct fdent fd_gadtools[] = {
	{ 30, "CreateGadgetA", "d0/a0/a1/a2" },
	{ 36, "FreeGadgets", "a0" },
	{ 42, "GT_SetGadgetAttrsA", "a0/a1/a2/a3" },
	{ 48, "CreateMenusA", "a0/a1" },
	{ 54, "FreeMenus", "a0" },
	{ 60, "LayoutMenuItemsA", "a0/a1/a2" },
	{ 66, "LayoutMenusA", "a0/a1/a2" },
	{ 72, "GT_GetIMsg", "a0" },
	{ 78, "GT_ReplyIMsg", "a1" },
	{ 84, "GT_RefreshWindow", "a0/a1" },
	{ 90, "GT_BeginRefresh", "a0" },
	{ 96, "GT_EndRefresh", "a0/d0" },
	{ 102, "GT_FilterIMsg", "a1" },
	{ 108, "GT_PostFilterIMsg", "a1" },
	{ 114, "CreateContext", "a0" },
	{ 120, "DrawBevelBoxA", "a0/d0/d1/d2/d3/a1" },
	{ 126, "GetVisualInfoA", "a0/a1" },
	{ 132, "FreeVisualInfo", "a0" },
	{ 174, "GT_GetGadgetAttrsA", "a0/a1/a2/a3" },
	{ 0, 0, 0 }
};

static const struct libimpl gadtools_impl[] = {
	{ "CreateGadgetA", gt_CreateGadgetA }, { "FreeGadgets", gt_FreeGadgets },
	{ "GT_SetGadgetAttrsA", gt_SetGadgetAttrsA },
	{ "GT_GetGadgetAttrsA", gt_GetGadgetAttrsA },
	{ "CreateMenusA", gt_CreateMenusA }, { "FreeMenus", gt_FreeMenus },
	{ "LayoutMenuItemsA", gt_True }, { "LayoutMenusA", gt_True },
	{ "GT_GetIMsg", gt_GetIMsg }, { "GT_ReplyIMsg", gt_ReplyIMsg },
	{ "GT_RefreshWindow", gt_RefreshWindow },
	{ "GT_BeginRefresh", gt_Nop }, { "GT_EndRefresh", gt_Nop },
	{ "GT_FilterIMsg", gt_Filter }, { "GT_PostFilterIMsg", gt_Filter },
	{ "CreateContext", gt_CreateContext },
	{ "DrawBevelBoxA", gt_DrawBevelBoxA },
	{ "GetVisualInfoA", gt_GetVisualInfoA },
	{ "FreeVisualInfo", gt_FreeVisualInfo },
	{ NULL, NULL }
};

struct amilib lib_gadtools = {
	.name = "gadtools.library", .version = 40, .revision = 4,
	.fd = fd_gadtools, .impl = gadtools_impl, .possize = 8,
};

