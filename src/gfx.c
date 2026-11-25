/*
 * graphics.library on a chunky window buffer or a planar BitMap.
 *
 * A RastPort is either a window's (its pixels are wbde's shared buffer of
 * pens; window coordinates include the border, so drawing is offset by
 * it and clipped to the inner area) or one the program set up on its own
 * BitMap (planar, as AllocBitMap/InitBitMap make them).  The screen's
 * RastPort draws nowhere.  Every drawing call reports the rectangle it
 * touched as damage; gui_flush() sends it before the program next waits.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"

struct Library *GfxBase;

struct surf {
	uint8_t *pix;
	int stride;
	struct BitMap *bm;
	int depth;
	int ox, oy;                     /* RastPort coords minus these */
	int cx0, cy0, cx1, cy1;         /* clip, surface coords */
	struct awin *a;
	int any, bx0, by0, bx1, by1;
	int16_t *rects;                 /* a layer's clip region, rp coords */
	int nrects;
};

/* ---- regions: rectangle lists kept here, keyed by the Region ---- */

struct rgn {
	uint32_t addr;
	int n, cap;
	int16_t *r;                     /* MinX, MinY, MaxX, MaxY */
	struct rgn *next;
};
static struct rgn *rgns;

static struct rgn *rgn_of(uint32_t addr)
{
	struct rgn *g;

	for (g = rgns; g; g = g->next)
		if (g->addr == addr)
			return g;
	g = calloc(1, sizeof(*g));
	g->addr = addr;
	g->next = rgns;
	rgns = g;
	return g;
}

static void rgn_add(struct rgn *g, int x0, int y0, int x1, int y1)
{
	if (x0 > x1 || y0 > y1)
		return;
	if (g->n == g->cap) {
		g->cap = g->cap ? g->cap * 2 : 16;
		g->r = realloc(g->r, g->cap * 8);
	}
	g->r[4 * g->n] = x0, g->r[4 * g->n + 1] = y0;
	g->r[4 * g->n + 2] = x1, g->r[4 * g->n + 3] = y1;
	g->n++;
}

static void rgn_bounds(struct rgn *g)
{
	int16_t *b = PTR(g->addr);
	int i;

	if (!g->n) {
		b[0] = b[1] = b[2] = b[3] = 0;
		return;
	}
	b[0] = b[1] = 32767, b[2] = b[3] = -32768;
	for (i = 0; i < g->n; i++) {
		if (g->r[4 * i] < b[0]) b[0] = g->r[4 * i];
		if (g->r[4 * i + 1] < b[1]) b[1] = g->r[4 * i + 1];
		if (g->r[4 * i + 2] > b[2]) b[2] = g->r[4 * i + 2];
		if (g->r[4 * i + 3] > b[3]) b[3] = g->r[4 * i + 3];
	}
}

/* the layer -> clip region map InstallClipRegion keeps */
struct lclip {
	uint32_t layer, region;
	struct lclip *next;
};
static struct lclip *lclips;

static uint32_t layer_region(uint32_t layer)
{
	struct lclip *l;

	for (l = lclips; l; l = l->next)
		if (l->layer == layer)
			return l->region;
	return 0;
}

static int in_region(struct surf *s, int x, int y)
{
	int i;

	for (i = 0; i < s->nrects; i++) {
		int16_t *r = s->rects + 4 * i;
		if (x >= r[0] && y >= r[1] && x <= r[2] && y <= r[3])
			return 1;
	}
	return 0;
}

static int surf_open(struct RastPort *rp, struct surf *s)
{
	struct awin *a = gui_by_rp(APTR_OF(rp));

	memset(s, 0, sizeof(*s));
	if (!rp)
		return 0;
	if (!a && rp->BitMap)
		for (a = gui_wins; a; a = a->next)
			if (APTR_OF(a->bm) == rp->BitMap)
				break;
	if (a) {
		if (!a->pix)
			return 0;
		s->a = a;
		s->pix = a->pix;
		s->stride = a->stride;
		s->depth = 8;
		if (!(a->w->Flags & 0x400)) {   /* not GIMMEZEROZERO */
			s->ox = a->bl;
			s->oy = a->bt;
		}
		s->cx1 = a->iw - 1;
		s->cy1 = a->ih - 1;
		if (rp->Layer && layer_region(rp->Layer)) {
			struct rgn *g = rgn_of(layer_region(rp->Layer));
			s->rects = g->r;
			s->nrects = g->n ? g->n : -1;   /* empty: nothing drawn */
		}
		return 1;
	}
	if (!rp->BitMap || (wb_screen && rp->BitMap == APTR_OF(&wb_screen->BitMap)))
		return 0;
	s->bm = PTR(rp->BitMap);
	s->depth = s->bm->Depth > 8 ? 8 : s->bm->Depth;
	s->cx1 = s->bm->BytesPerRow * 8 - 1;
	s->cy1 = s->bm->Rows - 1;
	if (rp->Layer) {
		/* a layer of the program's own on this bitmap: layer
		   coordinates, clipped to its bounds (Layer +16) */
		int16_t *b = (int16_t *)((uint8_t *)PTR(rp->Layer) + 16);
		s->ox = -b[0];
		s->oy = -b[1];
		if (b[0] > s->cx0) s->cx0 = b[0];
		if (b[1] > s->cy0) s->cy0 = b[1];
		if (b[2] < s->cx1) s->cx1 = b[2];
		if (b[3] < s->cy1) s->cy1 = b[3];
		if (layer_region(rp->Layer)) {
			struct rgn *g = rgn_of(layer_region(rp->Layer));
			s->rects = g->r;
			s->nrects = g->n ? g->n : -1;
		}
	}
	return 1;
}

static void surf_close(struct surf *s)
{
	if (s->a && s->any)
		gui_damage(s->a, s->bx0, s->by0, s->bx1, s->by1);
}

static inline void touch(struct surf *s, int x0, int y0, int x1, int y1)
{
	if (!s->any) {
		s->any = 1;
		s->bx0 = x0, s->by0 = y0, s->bx1 = x1, s->by1 = y1;
		return;
	}
	if (x0 < s->bx0) s->bx0 = x0;
	if (y0 < s->by0) s->by0 = y0;
	if (x1 > s->bx1) s->bx1 = x1;
	if (y1 > s->by1) s->by1 = y1;
}

/* surface coordinates, already clipped */
static inline int sget(struct surf *s, int x, int y)
{
	int p, v = 0, byte = x >> 3, bit = 7 - (x & 7);

	if (s->pix)
		return s->pix[y * s->stride + x];
	for (p = 0; p < s->depth; p++) {
		uint32_t pl = s->bm->Planes[p];
		if (pl == 0xffffffff)
			v |= 1 << p;
		else if (pl && (((uint8_t *)PTR(pl))[y * s->bm->BytesPerRow + byte] >> bit) & 1)
			v |= 1 << p;
	}
	return v;
}

static inline void sset(struct surf *s, int x, int y, int pen)
{
	int p, byte = x >> 3;
	uint8_t bit = 0x80 >> (x & 7);

	if (s->nrects && (s->nrects < 0 || !in_region(s, x + s->ox, y + s->oy)))
		return;
	if (s->pix) {
		s->pix[y * s->stride + x] = pen;
		return;
	}
	for (p = 0; p < s->depth; p++) {
		uint32_t pl = s->bm->Planes[p];
		uint8_t *b;
		if (!pl || pl == 0xffffffff)
			continue;
		b = (uint8_t *)PTR(pl) + y * s->bm->BytesPerRow + byte;
		if ((pen >> p) & 1)
			*b |= bit;
		else
			*b &= ~bit;
	}
}

/* one pixel through the RastPort's draw mode; fg = foreground bit */
static inline void plot(struct surf *s, struct RastPort *rp, int x, int y, int fg)
{
	int mode = rp->DrawMode;

	x -= s->ox;
	y -= s->oy;
	if (x < s->cx0 || y < s->cy0 || x > s->cx1 || y > s->cy1)
		return;
	if (mode & INVERSVID)
		fg = !fg;
	if (mode & COMPLEMENT) {
		if (fg)
			sset(s, x, y, sget(s, x, y) ^ rp->Mask);
		else
			return;
	} else if (fg)
		sset(s, x, y, (uint8_t)rp->FgPen);
	else if (mode & JAM2)
		sset(s, x, y, (uint8_t)rp->BgPen);
	else
		return;
	touch(s, x, y, x, y);
}

/* a filled rectangle in RastPort coords with one pen, no draw mode */
static void fill_pen(struct surf *s, int x0, int y0, int x1, int y1, int pen)
{
	int x, y;

	x0 -= s->ox, x1 -= s->ox, y0 -= s->oy, y1 -= s->oy;
	if (x0 < s->cx0) x0 = s->cx0;
	if (y0 < s->cy0) y0 = s->cy0;
	if (x1 > s->cx1) x1 = s->cx1;
	if (y1 > s->cy1) y1 = s->cy1;
	if (x0 > x1 || y0 > y1)
		return;
	if (s->pix && !s->nrects)
		for (y = y0; y <= y1; y++)
			memset(s->pix + y * s->stride + x0, pen, x1 - x0 + 1);
	else
		for (y = y0; y <= y1; y++)
			for (x = x0; x <= x1; x++)
				sset(s, x, y, pen);
	touch(s, x0, y0, x1, y1);
}

/* RectFill semantics: FgPen, draw mode, AreaPtrn */
static void rect_rp(struct surf *s, struct RastPort *rp, int x0, int y0, int x1, int y1)
{
	int x, y, rows = rp->AreaPtSz > 0 ? 1 << rp->AreaPtSz : 1;
	uint16_t *pat = rp->AreaPtrn ? PTR(rp->AreaPtrn) : NULL;

	if (!pat && !(rp->DrawMode & (COMPLEMENT | INVERSVID))) {
		fill_pen(s, x0, y0, x1, y1, (uint8_t)rp->FgPen);
		return;
	}
	for (y = y0; y <= y1; y++) {
		uint16_t row = pat ? pat[y & (rows - 1)] : 0xffff;
		for (x = x0; x <= x1; x++)
			plot(s, rp, x, y, (row >> (15 - (x & 15))) & 1);
	}
}

void gfx_rectfill(struct RastPort *rp, int x0, int y0, int x1, int y1, int pen)
{
	struct surf s;

	if (!surf_open(rp, &s))
		return;
	if (pen < 0)
		rect_rp(&s, rp, x0, y0, x1, y1);
	else
		fill_pen(&s, x0, y0, x1, y1, pen);
	surf_close(&s);
}

void gfx_pixel(struct RastPort *rp, int x, int y, int pen)
{
	gfx_rectfill(rp, x, y, x, y, pen);
}

void gfx_complement(struct RastPort *rp, int x0, int y0, int x1, int y1)
{
	struct surf s;
	int x, y;

	if (!surf_open(rp, &s))
		return;
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++) {
			int sx = x - s.ox, sy = y - s.oy;
			if (sx < s.cx0 || sy < s.cy0 || sx > s.cx1 || sy > s.cy1)
				continue;
			sset(&s, sx, sy, sget(&s, sx, sy) ^ 0xff);
			touch(&s, sx, sy, sx, sy);
		}
	surf_close(&s);
}

static void line_s(struct surf *s, struct RastPort *rp, int x0, int y0, int x1, int y1)
{
	int dx = abs(x1 - x0), dy = -abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
	uint16_t pat = rp->LinePtrn ? rp->LinePtrn : 0xffff;
	int n = 0;

	for (;;) {
		plot(s, rp, x0, y0, (pat >> (15 - (n++ & 15))) & 1);
		if (x0 == x1 && y0 == y1)
			break;
		if (2 * e >= dy) {
			e += dy;
			x0 += sx;
		}
		if (2 * e <= dx) {
			e += dx;
			y0 += sy;
		}
	}
}

void gfx_line(struct RastPort *rp, int x0, int y0, int x1, int y1)
{
	struct surf s;

	if (surf_open(rp, &s)) {
		line_s(&s, rp, x0, y0, x1, y1);
		surf_close(&s);
	}
	rp->cp_x = x1;
	rp->cp_y = y1;
}

/* ---- text ---- */

struct TextFont *gfx_font_for(struct TextAttr *ta)
{
	(void)ta;
	if (!gui_topaz)
		gui_connect();
	return gui_topaz;       /* the only font there is: topaz from wbde */
}

static struct TextFont *rp_font(struct RastPort *rp)
{
	if (rp->Font)
		return PTR(rp->Font);
	if (!gui_topaz)
		gui_connect();
	return gui_topaz;
}

static int glyph(struct TextFont *tf, int c, int *off, int *w, int *kern, int *space)
{
	int lo = tf->tf_LoChar, hi = tf->tf_HiChar, i;
	uint32_t loc;

	i = (c < lo || c > hi) ? hi - lo + 1 : c - lo;
	loc = ((uint32_t *)PTR(tf->tf_CharLoc))[i];
	*off = loc >> 16;
	*w = loc & 0xffff;
	*kern = tf->tf_CharKern ? ((int16_t *)PTR(tf->tf_CharKern))[i] : 0;
	*space = tf->tf_CharSpace ? ((int16_t *)PTR(tf->tf_CharSpace))[i] :
		 tf->tf_XSize;
	return 1;
}

int gfx_textlength(struct RastPort *rp, const char *str, int n)
{
	struct TextFont *tf = rp ? rp_font(rp) : gui_topaz;
	int i, len = 0, off, w, kern, space;

	if (!tf)
		return n * 8;
	for (i = 0; i < n; i++) {
		glyph(tf, (unsigned char)str[i], &off, &w, &kern, &space);
		len += space + (rp ? rp->TxSpacing : 0);
	}
	if (rp && (rp->AlgoStyle & 2))          /* FSF_BOLD */
		len += n * tf->tf_BoldSmear;
	return len;
}

void gfx_text(struct RastPort *rp, const char *str, int n)
{
	struct TextFont *tf = rp_font(rp);
	struct surf s;
	const uint8_t *data;
	int i, x = rp->cp_x, y0, bold, under;

	if (!tf)
		return;
	if (!surf_open(rp, &s)) {
		rp->cp_x += gfx_textlength(rp, str, n);
		return;
	}
	data = PTR(tf->tf_CharData);
	y0 = rp->cp_y - tf->tf_Baseline;
	bold = (rp->AlgoStyle & 2) ? tf->tf_BoldSmear : 0;
	under = rp->AlgoStyle & 1;
	for (i = 0; i < n; i++) {
		int off, w, kern, space, row, col, adv;
		glyph(tf, (unsigned char)str[i], &off, &w, &kern, &space);
		adv = space + rp->TxSpacing + bold;
		if ((rp->DrawMode & JAM2) && !(rp->DrawMode & COMPLEMENT)) {
			int bgpen = (rp->DrawMode & INVERSVID) ? rp->FgPen : rp->BgPen;
			fill_pen(&s, x, y0, x + adv - 1, y0 + tf->tf_YSize - 1,
				 (uint8_t)bgpen);
		}
		for (row = 0; row < tf->tf_YSize; row++) {
			const uint8_t *r = data + row * tf->tf_Modulo;
			for (col = 0; col < w; col++) {
				int bit = off + col, b;
				if (!((r[bit >> 3] >> (7 - (bit & 7))) & 1))
					continue;
				for (b = 0; b <= bold; b++) {
					int px = x + kern + col + b;
					if (rp->DrawMode & COMPLEMENT)
						plot(&s, rp, px, y0 + row, 1);
					else {
						int sx = px - s.ox, sy = y0 + row - s.oy;
						int pen = (rp->DrawMode & INVERSVID) ?
							  rp->BgPen : rp->FgPen;
						if (sx < s.cx0 || sy < s.cy0 ||
						    sx > s.cx1 || sy > s.cy1)
							continue;
						sset(&s, sx, sy, (uint8_t)pen);
						touch(&s, sx, sy, sx, sy);
					}
				}
			}
		}
		if (under)
			fill_pen(&s, x, rp->cp_y + 1, x + adv - 1, rp->cp_y + 1,
				 (uint8_t)rp->FgPen);
		x += adv;
	}
	rp->cp_x = x;
	surf_close(&s);
}

/* ---- images, borders, IntuiText (Intuition's, but pure drawing) ---- */

void gfx_drawimage(struct RastPort *rp, struct Image *im, int x0, int y0)
{
	struct surf s;

	if (im && im->Depth == -1) {           /* a BOOPSI image */
		boopsi_drawimage(rp, im, x0, y0, 0);
		return;
	}
	if (!surf_open(rp, &s))
		return;
	for (; im; im = PTR(im->NextImage)) {
		int words = (im->Width + 15) / 16, x, y, p, plane;
		const uint8_t *data = PTR(im->ImageData);
		int planesize = words * 2 * im->Height;
		int pick[8], onoff = im->PlaneOnOff;

		for (p = 0, plane = 0; p < 8; p++)
			pick[p] = (im->PlanePick >> p) & 1 && plane < im->Depth ?
				  plane++ : -1;
		for (y = 0; y < im->Height; y++)
			for (x = 0; x < im->Width; x++) {
				int pen = 0, sx, sy;
				for (p = 0; p < s.depth; p++) {
					if (pick[p] >= 0 && data) {
						const uint8_t *b = data + pick[p] * planesize +
								   y * words * 2 + (x >> 3);
						pen |= ((*b >> (7 - (x & 7))) & 1) << p;
					} else
						pen |= ((onoff >> p) & 1) << p;
				}
				sx = x0 + im->LeftEdge + x - s.ox;
				sy = y0 + im->TopEdge + y - s.oy;
				if (sx < s.cx0 || sy < s.cy0 || sx > s.cx1 || sy > s.cy1)
					continue;
				sset(&s, sx, sy, pen);
				touch(&s, sx, sy, sx, sy);
			}
	}
	surf_close(&s);
}

void gfx_drawborder(struct RastPort *rp, struct Border *b, int x0, int y0)
{
	int8_t fg = rp->FgPen, dm = rp->DrawMode;
	struct surf s;

	if (!surf_open(rp, &s))
		return;
	for (; b; b = PTR(b->NextBorder)) {
		int16_t *xy = PTR(b->XY);
		int i, lx = 0, ly = 0;
		rp->FgPen = b->FrontPen;
		rp->DrawMode = b->DrawMode & ~JAM2;
		for (i = 0; i < b->Count; i++) {
			int x = x0 + b->LeftEdge + xy[2 * i];
			int y = y0 + b->TopEdge + xy[2 * i + 1];
			if (i)
				line_s(&s, rp, lx, ly, x, y);
			else if (b->Count == 1)
				plot(&s, rp, x, y, 1);
			lx = x, ly = y;
		}
	}
	rp->FgPen = fg;
	rp->DrawMode = dm;
	surf_close(&s);
}

void gfx_printitext(struct RastPort *rp, struct IntuiText *it, int x0, int y0)
{
	int8_t fg = rp->FgPen, bg = rp->BgPen, dm = rp->DrawMode;
	int16_t cx = rp->cp_x, cy = rp->cp_y;
	APTR font = rp->Font;

	for (; it; it = PTR(it->NextText)) {
		struct TextFont *tf = it->ITextFont ?
				      gfx_font_for(PTR(it->ITextFont)) : rp_font(rp);
		const char *t = it->IText ? PTR(it->IText) : "";
		if (tf)
			rp->Font = APTR_OF(tf);
		rp->FgPen = it->FrontPen;
		rp->BgPen = it->BackPen;
		rp->DrawMode = it->DrawMode;
		rp->cp_x = x0 + it->LeftEdge;
		rp->cp_y = y0 + it->TopEdge + (tf ? tf->tf_Baseline : 6);
		gfx_text(rp, t, strlen(t));
	}
	rp->FgPen = fg;
	rp->BgPen = bg;
	rp->DrawMode = dm;
	rp->cp_x = cx;
	rp->cp_y = cy;
	rp->Font = font;
}

int gfx_itextlength(struct IntuiText *it)
{
	struct RastPort rp;
	const char *t = it->IText ? PTR(it->IText) : "";

	gfx_init_rp(&rp);
	if (it->ITextFont)
		rp.Font = APTR_OF(gfx_font_for(PTR(it->ITextFont)));
	return gfx_textlength(&rp, t, strlen(t));
}

/* a 3D box in the Workbench's pens: shine top-left, shadow bottom-right */
void gfx_bevel(struct RastPort *rp, int x, int y, int w, int h, int recessed)
{
	int shine = gui_pens[WBP_SHINEPEN], shadow = gui_pens[WBP_SHADOWPEN];
	int a = recessed ? shadow : shine, b = recessed ? shine : shadow;

	if (w < 2 || h < 2)
		return;
	gfx_rectfill(rp, x, y, x + w - 1, y, a);
	gfx_rectfill(rp, x, y, x, y + h - 1, a);
	gfx_rectfill(rp, x + 1, y + h - 1, x + w - 1, y + h - 1, b);
	gfx_rectfill(rp, x + w - 1, y + 1, x + w - 1, y + h - 1, b);
}

void gfx_init_rp(struct RastPort *rp)
{
	memset(rp, 0, sizeof(*rp));
	rp->Mask = 0xff;
	rp->FgPen = -1;
	rp->AOlPen = -1;
	rp->DrawMode = JAM2;
	rp->LinePtrn = 0xffff;
	rp->Flags = 0x01;                       /* FRST_DOT */
	rp->PenWidth = rp->PenHeight = 1;
	if (gui_topaz) {
		rp->Font = APTR_OF(gui_topaz);
		rp->TxHeight = gui_topaz->tf_YSize;
		rp->TxWidth = gui_topaz->tf_XSize;
		rp->TxBaseline = gui_topaz->tf_Baseline;
	}
}

/* ---- the library's vectors ---- */

static struct RastPort *RP(uint32_t a) { return PTR(a); }

static void g_InitRastPort(struct frame *f) { gfx_init_rp(RP(A(1))); }
static void g_SetAPen(struct frame *f) { RP(A(1))->FgPen = D(0); }
static void g_SetBPen(struct frame *f) { RP(A(1))->BgPen = D(0); }
static void g_SetDrMd(struct frame *f) { RP(A(1))->DrawMode = D(0); }

static void g_SetABPenDrMd(struct frame *f)
{
	RP(A(1))->FgPen = D(0);
	RP(A(1))->BgPen = D(1);
	RP(A(1))->DrawMode = D(2);
}

static void g_GetAPen(struct frame *f) { RET((uint8_t)RP(A(0))->FgPen); }
static void g_GetBPen(struct frame *f) { RET((uint8_t)RP(A(0))->BgPen); }
static void g_GetDrMd(struct frame *f) { RET((uint8_t)RP(A(0))->DrawMode); }
static void g_GetOutlinePen(struct frame *f) { RET((uint8_t)RP(A(0))->AOlPen); }

static void g_SetOutlinePen(struct frame *f)
{
	RET((uint8_t)RP(A(0))->AOlPen);
	RP(A(0))->AOlPen = D(0);
	RP(A(0))->Flags |= 0x08;                /* AREAOUTLINE */
}

static void g_SetWriteMask(struct frame *f)
{
	RP(A(0))->Mask = D(0);
	RET(1);
}

static void g_SetMaxPen(struct frame *f) { (void)f; }

static void g_Move(struct frame *f)
{
	RP(A(1))->cp_x = D(0);
	RP(A(1))->cp_y = D(1);
}

static void g_Draw(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	gfx_line(rp, rp->cp_x, rp->cp_y, (int16_t)D(0), (int16_t)D(1));
}

static void g_PolyDraw(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	int16_t *xy = PTR(A(0));
	int i;

	for (i = 0; i < (int16_t)D(0); i++)
		gfx_line(rp, rp->cp_x, rp->cp_y, xy[2 * i], xy[2 * i + 1]);
}

static void g_RectFill(struct frame *f)
{
	gfx_rectfill(RP(A(1)), (int16_t)D(0), (int16_t)D(1), (int16_t)D(2),
		     (int16_t)D(3), -1);
}

static void g_WritePixel(struct frame *f)
{
	struct surf s;

	RET(0);
	if (!surf_open(RP(A(1)), &s))
		return;
	plot(&s, RP(A(1)), (int16_t)D(0), (int16_t)D(1), 1);
	surf_close(&s);
}

static void g_ReadPixel(struct frame *f)
{
	struct surf s;
	int x, y;

	RET(-1);
	if (!surf_open(RP(A(1)), &s))
		return;
	x = (int16_t)D(0) - s.ox;
	y = (int16_t)D(1) - s.oy;
	if (x >= s.cx0 && y >= s.cy0 && x <= s.cx1 && y <= s.cy1)
		RET(sget(&s, x, y));
}

static void g_SetRast(struct frame *f)
{
	struct surf s;

	if (!surf_open(RP(A(1)), &s))
		return;
	fill_pen(&s, s.cx0 + s.ox, s.cy0 + s.oy, s.cx1 + s.ox, s.cy1 + s.oy,
		 (uint8_t)D(0));
	surf_close(&s);
}

static void g_EraseRect(struct frame *f)
{
	gfx_rectfill(RP(A(1)), (int16_t)D(0), (int16_t)D(1), (int16_t)D(2),
		     (int16_t)D(3), 0);
}

static void g_ScrollRaster(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	int dx = (int16_t)D(0), dy = (int16_t)D(1);
	int x0 = (int16_t)D(2), y0 = (int16_t)D(3);
	int x1 = (int16_t)D(4), y1 = (int16_t)D(5);
	int w = x1 - x0 + 1, h = y1 - y0 + 1, x, y;
	struct surf s;
	uint8_t *tmp;

	if (w <= 0 || h <= 0 || !surf_open(rp, &s))
		return;
	tmp = malloc(w * h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int sx = x0 + x + dx - s.ox, sy = y0 + y + dy - s.oy;
			int in = x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h &&
				 sx >= s.cx0 && sy >= s.cy0 && sx <= s.cx1 && sy <= s.cy1;
			tmp[y * w + x] = in ? sget(&s, sx, sy) : (uint8_t)rp->BgPen;
		}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int sx = x0 + x - s.ox, sy = y0 + y - s.oy;
			if (sx < s.cx0 || sy < s.cy0 || sx > s.cx1 || sy > s.cy1)
				continue;
			sset(&s, sx, sy, tmp[y * w + x]);
		}
	touch(&s, x0 - s.ox, y0 - s.oy, x1 - s.ox, y1 - s.oy);
	free(tmp);
	surf_close(&s);
}

/* integer square root, for ellipses without the FPU (the 060 has no
   transcendental instructions) */
static int isqrt(long v)
{
	long r = 0, bit = 1L << 30;

	if (v <= 0)
		return 0;
	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else
			r >>= 1;
		bit >>= 2;
	}
	return r;
}

/* half width of an ellipse a x b at row dy */
static int ehw(int a, int b, int dy)
{
	if (b <= 0)
		return a;
	return isqrt((long)a * a * ((long)b * b - (long)dy * dy) / ((long)b * b));
}

static void g_DrawEllipse(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	int cx = (int16_t)D(0), cy = (int16_t)D(1);
	int a = (int16_t)D(2), b = (int16_t)D(3), dy, prev = -1;
	struct surf s;

	if (!surf_open(rp, &s))
		return;
	for (dy = -b; dy <= b; dy++) {
		int w = ehw(a, b, dy), from = prev < 0 ? w : prev, x;
		/* fill between this row's edge and the previous one's */
		int lo = from < w ? from : w, hi = from < w ? w : from;
		if (prev < 0 || dy == b)
			lo = 0;
		if (dy > 0 && prev >= 0) {      /* lower half: towards the last */
			lo = w < prev ? w : prev;
			hi = w < prev ? prev : w;
		}
		for (x = lo; x <= hi; x++) {
			plot(&s, rp, cx + x, cy + dy, 1);
			if (x)
				plot(&s, rp, cx - x, cy + dy, 1);
		}
		prev = w;
	}
	surf_close(&s);
}

/* ---- area fills: vertices kept here per RastPort ---- */

struct area {
	struct RastPort *rp;
	int n, cap;
	int16_t *x, *y;
	int *start;                     /* polygon start flags */
	struct area *next;
};
static struct area *areas;

static struct area *area_of(struct RastPort *rp)
{
	struct area *a;

	for (a = areas; a; a = a->next)
		if (a->rp == rp)
			return a;
	a = calloc(1, sizeof(*a));
	a->rp = rp;
	a->next = areas;
	areas = a;
	return a;
}

static void area_add(struct area *a, int x, int y, int start)
{
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 64;
		a->x = realloc(a->x, a->cap * 2);
		a->y = realloc(a->y, a->cap * 2);
		a->start = realloc(a->start, a->cap * sizeof(int));
	}
	a->x[a->n] = x;
	a->y[a->n] = y;
	a->start[a->n] = start;
	a->n++;
}

static void g_InitArea(struct frame *f)
{
	struct AreaInfo *ai = PTR(A(0));

	memset(ai, 0, sizeof(*ai));
	ai->VctrTbl = ai->VctrPtr = A(1);
	ai->MaxCount = D(0);
}

static void g_AreaMove(struct frame *f)
{
	area_add(area_of(RP(A(1))), (int16_t)D(0), (int16_t)D(1), 1);
	RET(0);
}

static void g_AreaDraw(struct frame *f)
{
	area_add(area_of(RP(A(1))), (int16_t)D(0), (int16_t)D(1), 0);
	RET(0);
}

static void g_AreaEllipse(struct frame *f)
{
	struct area *a = area_of(RP(A(1)));
	int cx = (int16_t)D(0), cy = (int16_t)D(1), ra = (int16_t)D(2),
	    rb = (int16_t)D(3), dy;

	/* down the right side, up the left: a polygon a row per vertex */
	for (dy = -rb; dy <= rb; dy++)
		area_add(a, cx + ehw(ra, rb, dy), cy + dy, dy == -rb);
	for (dy = rb; dy >= -rb; dy--)
		area_add(a, cx - ehw(ra, rb, dy), cy + dy, 0);
	RET(0);
}

static void g_AreaEnd(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	struct area *a = area_of(rp);
	struct surf s;
	int miny = 32767, maxy = -32768, i, y;

	RET(0);
	if (!a->n || !surf_open(rp, &s)) {
		a->n = 0;
		return;
	}
	for (i = 0; i < a->n; i++) {
		if (a->y[i] < miny) miny = a->y[i];
		if (a->y[i] > maxy) maxy = a->y[i];
	}
	for (y = miny; y <= maxy; y++) {
		int xs[512], nx = 0, j, k;
		for (i = 0; i < a->n; i++) {
			/* the edge from i to the next vertex of its polygon */
			int nxt = (i + 1 < a->n && !a->start[i + 1]) ? i + 1 : i;
			int y0, y1, x0, x1;
			if (nxt == i) {         /* closing edge back to the start */
				for (nxt = i; nxt > 0 && !a->start[nxt]; nxt--)
					;
			}
			y0 = a->y[i], y1 = a->y[nxt], x0 = a->x[i], x1 = a->x[nxt];
			if (y0 == y1)
				continue;
			if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
				if (nx < 512)
					xs[nx++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
			}
		}
		for (j = 1; j < nx; j++)
			for (k = j; k > 0 && xs[k - 1] > xs[k]; k--) {
				int t = xs[k];
				xs[k] = xs[k - 1];
				xs[k - 1] = t;
			}
		for (j = 0; j + 1 < nx; j += 2)
			rect_rp(&s, rp, xs[j], y, xs[j + 1], y);
	}
	if (rp->Flags & 0x08) {                 /* AREAOUTLINE */
		int8_t fg = rp->FgPen;
		rp->FgPen = rp->AOlPen;
		for (i = 0; i < a->n; i++) {
			int nxt = (i + 1 < a->n && !a->start[i + 1]) ? i + 1 : -1, st;
			if (nxt < 0) {
				for (st = i; st > 0 && !a->start[st]; st--)
					;
				nxt = st;
			}
			line_s(&s, rp, a->x[i], a->y[i], a->x[nxt], a->y[nxt]);
		}
		rp->FgPen = fg;
	}
	a->n = 0;
	surf_close(&s);
}

static void g_InitTmpRas(struct frame *f)
{
	uint32_t *t = PTR(A(0));

	t[0] = A(1);
	t[1] = D(0);
	RET(A(0));
}

static void g_Flood(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	struct surf s;
	int mode = D(2), x = (int16_t)D(0), y = (int16_t)D(1), old, n = 0;
	int *stack;

	RET(0);
	if (!surf_open(rp, &s))
		return;
	x -= s.ox, y -= s.oy;
	if (x < s.cx0 || y < s.cy0 || x > s.cx1 || y > s.cy1) {
		surf_close(&s);
		return;
	}
	old = sget(&s, x, y);
	if (!mode && old == (uint8_t)rp->FgPen) {
		surf_close(&s);
		return;
	}
	stack = malloc(sizeof(int) * 2 * 65536);
	stack[n++] = x, stack[n++] = y;
	while (n) {
		int py = stack[--n], px = stack[--n], v;
		if (px < s.cx0 || py < s.cy0 || px > s.cx1 || py > s.cy1)
			continue;
		v = sget(&s, px, py);
		/* mode 1: fill pixels of the colour under the start point;
		   mode 0: fill until the outline pen */
		if (mode ? v != old : (v == (uint8_t)rp->AOlPen ||
				       v == (uint8_t)rp->FgPen))
			continue;
		sset(&s, px, py, (uint8_t)rp->FgPen);
		touch(&s, px, py, px, py);
		if (n < 2 * 65536 - 8) {
			stack[n++] = px + 1, stack[n++] = py;
			stack[n++] = px - 1, stack[n++] = py;
			stack[n++] = px, stack[n++] = py + 1;
			stack[n++] = px, stack[n++] = py - 1;
		}
	}
	free(stack);
	surf_close(&s);
	RET(1);
}

/* ---- fonts ---- */

static void g_OpenFont(struct frame *f) { RET(gfx_font_for(PTR(A(0)))); }
static void g_CloseFont(struct frame *f) { (void)f; }

static void g_SetFont(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	struct TextFont *tf = PTR(A(0));

	if (!tf)
		return;
	rp->Font = A(0);
	rp->TxHeight = tf->tf_YSize;
	rp->TxWidth = tf->tf_XSize;
	rp->TxBaseline = tf->tf_Baseline;
	RET(1);
}

static void g_AskFont(struct frame *f)
{
	struct TextFont *tf = rp_font(RP(A(1)));
	struct TextAttr *ta = PTR(A(0));

	if (!tf)
		return;
	ta->ta_Name = tf->tf_Message.mn_Node.ln_Name;
	ta->ta_YSize = tf->tf_YSize;
	ta->ta_Style = tf->tf_Style;
	ta->ta_Flags = tf->tf_Flags;
}

static void g_SetSoftStyle(struct frame *f)
{
	struct RastPort *rp = RP(A(1));

	rp->AlgoStyle = (rp->AlgoStyle & ~D(1)) | (D(0) & D(1) & 7);
	RET(rp->AlgoStyle);
}

static void g_AskSoftStyle(struct frame *f) { RET(7); }

static void g_Text(struct frame *f)
{
	gfx_text(RP(A(1)), PTR(A(0)), (int16_t)D(0));
	RET(0);
}

static void g_TextLength(struct frame *f)
{
	RET(gfx_textlength(RP(A(1)), PTR(A(0)), (int16_t)D(0)));
}

/* struct TextExtent { WORD te_Width, te_Height; Rectangle te_Extent } */
static void g_TextExtent(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	struct TextFont *tf = rp_font(rp);
	int16_t *te = PTR(A(2));
	int w = gfx_textlength(rp, PTR(A(0)), (int16_t)D(0));
	int h = tf ? tf->tf_YSize : 8, base = tf ? tf->tf_Baseline : 6;

	te[0] = w;
	te[1] = h;
	te[2] = 0;
	te[3] = -base;
	te[4] = w > 0 ? w - 1 : 0;
	te[5] = h - base - 1;
}

static void g_TextFit(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	const char *s = PTR(A(0));
	int n = (uint16_t)D(0), dir = (int16_t)D(1), maxw = (uint16_t)D(2), i;
	int16_t *te = PTR(A(2));
	struct TextFont *tf = rp_font(rp);

	for (i = 0; i < n; i++) {
		const char *p = dir < 0 ? s - i : s;
		if (gfx_textlength(rp, p, i + 1) > maxw)
			break;
	}
	if (te) {
		int w = gfx_textlength(rp, s, i);
		te[0] = w;
		te[1] = tf ? tf->tf_YSize : 8;
		te[2] = 0;
		te[3] = tf ? -tf->tf_Baseline : -6;
		te[4] = w ? w - 1 : 0;
		te[5] = te[1] + te[3] - 1;
	}
	RET(i);
}

static void g_ClearEOL(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	struct TextFont *tf = rp_font(rp);
	int y = rp->cp_y - (tf ? tf->tf_Baseline : 6);

	gfx_rectfill(rp, rp->cp_x, y, 32767, y + (tf ? tf->tf_YSize : 8) - 1,
		     (uint8_t)rp->BgPen);
}

static void g_ClearScreen(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	struct TextFont *tf = rp_font(rp);
	int y = rp->cp_y - (tf ? tf->tf_Baseline : 6);

	g_ClearEOL(f);
	gfx_rectfill(rp, 0, y + (tf ? tf->tf_YSize : 8), 32767, 32767,
		     (uint8_t)rp->BgPen);
}

/* ---- bitmaps and blits ---- */

static void g_InitBitMap(struct frame *f)
{
	struct BitMap *bm = PTR(A(0));

	memset(bm, 0, 8);
	bm->Depth = D(0);
	bm->BytesPerRow = (((int16_t)D(1) + 15) >> 4) << 1;
	bm->Rows = D(2);
}

static void g_AllocRaster(struct frame *f)
{
	RET(amem_alloc((((D(0) + 15) >> 4) << 1) * D(1)));
}

static void g_FreeRaster(struct frame *f) { amem_free(PTR(A(0))); }

static void g_AllocBitMap(struct frame *f)
{
	int w = D(0), h = D(1), depth = D(2), bpr = ((w + 15) >> 4) << 1, p;
	struct BitMap *bm = amem_alloc(sizeof(*bm));

	bm->BytesPerRow = bpr;
	bm->Rows = h;
	bm->Depth = depth > 8 ? 8 : depth;
	for (p = 0; p < bm->Depth; p++)
		bm->Planes[p] = APTR_OF(amem_alloc(bpr * h + 2));
	RET(bm);
}

static void g_FreeBitMap(struct frame *f)
{
	struct BitMap *bm = PTR(A(0));
	int p;

	if (!bm)
		return;
	for (p = 0; p < bm->Depth && p < 8; p++)
		if (bm->Planes[p] && bm->Planes[p] != 0xffffffff)
			amem_free(PTR(bm->Planes[p]));
	amem_free(bm);
}

static void g_GetBitMapAttr(struct frame *f)
{
	struct BitMap *bm = PTR(A(0));

	switch (D(1)) {
	case 0: RET(bm->Rows); break;                   /* BMA_HEIGHT */
	case 4: RET(bm->Depth); break;                  /* BMA_DEPTH */
	case 8: RET(bm->BytesPerRow * 8); break;        /* BMA_WIDTH */
	default: RET(0);
	}
}

static void g_BltClear(struct frame *f)
{
	uint32_t n = D(0);

	if (D(1) & 2)
		n = (n >> 16) * (n & 0xffff);
	memset(PTR(A(1)), 0, n);
}

/* minterm with the A channel all ones: result bit from bits 4..7 */
static inline int mint(int m, int src, int dst, int depth)
{
	int r = 0, p;

	if (m == 0xc0)
		return src;
	for (p = 0; p < depth; p++) {
		int b = (src >> p) & 1, c = (dst >> p) & 1;
		r |= ((m >> (4 + 2 * b + c)) & 1) << p;
	}
	return r;
}

/* a BitMap that is not a window's, as a surface */
static int surf_bm(struct BitMap *bm, struct surf *s)
{
	struct RastPort rp;

	memset(&rp, 0, sizeof(rp));
	rp.BitMap = APTR_OF(bm);
	return surf_open(&rp, s);
}

static void blit(struct surf *src, int sx, int sy, struct surf *dst, int dx,
		 int dy, int w, int h, int minterm, const uint8_t *mask, int mbpr)
{
	uint8_t *tmp;
	int x, y;

	if (w <= 0 || h <= 0)
		return;
	tmp = malloc(w * h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int px = sx + x - src->ox, py = sy + y - src->oy;
			tmp[y * w + x] = (px >= src->cx0 && py >= src->cy0 &&
					  px <= src->cx1 && py <= src->cy1) ?
					 sget(src, px, py) : 0;
		}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int px = dx + x - dst->ox, py = dy + y - dst->oy;
			if (px < dst->cx0 || py < dst->cy0 || px > dst->cx1 ||
			    py > dst->cy1)
				continue;
			if (mask && !((mask[y * mbpr + ((sx + x) >> 3)] >>
				       (7 - ((sx + x) & 7))) & 1))
				continue;
			sset(dst, px, py, mint(minterm, tmp[y * w + x],
					       minterm == 0xc0 ? 0 : sget(dst, px, py),
					       dst->depth));
		}
	touch(dst, dx - dst->ox, dy - dst->oy, dx + w - 1 - dst->ox,
	      dy + h - 1 - dst->oy);
	free(tmp);
}

static void g_BltBitMap(struct frame *f)
{
	struct surf s, d;

	RET(0);
	if (!surf_bm(PTR(A(0)), &s) || !surf_bm(PTR(A(1)), &d))
		return;
	blit(&s, (int16_t)D(0), (int16_t)D(1), &d, (int16_t)D(2), (int16_t)D(3),
	     (int16_t)D(4), (int16_t)D(5), D(6) & 0xff, NULL, 0);
	surf_close(&d);
	RET(8);
}

static void g_BltBitMapRastPort(struct frame *f)
{
	struct surf s, d;

	if (!surf_bm(PTR(A(0)), &s) || !surf_open(RP(A(1)), &d))
		return;
	blit(&s, (int16_t)D(0), (int16_t)D(1), &d, (int16_t)D(2), (int16_t)D(3),
	     (int16_t)D(4), (int16_t)D(5), D(6) & 0xff, NULL, 0);
	surf_close(&d);
}

static void g_BltMaskBitMapRastPort(struct frame *f)
{
	struct BitMap *bm = PTR(A(0));
	struct surf s, d;

	if (!surf_bm(bm, &s) || !surf_open(RP(A(1)), &d))
		return;
	blit(&s, (int16_t)D(0), (int16_t)D(1), &d, (int16_t)D(2), (int16_t)D(3),
	     (int16_t)D(4), (int16_t)D(5), 0xc0, PTR(A(2)), bm->BytesPerRow);
	surf_close(&d);
}

static void g_ClipBlit(struct frame *f)
{
	struct surf s, d;

	if (!surf_open(RP(A(0)), &s) || !surf_open(RP(A(1)), &d))
		return;
	blit(&s, (int16_t)D(0), (int16_t)D(1), &d, (int16_t)D(2), (int16_t)D(3),
	     (int16_t)D(4), (int16_t)D(5), D(6) & 0xff, NULL, 0);
	surf_close(&d);
}

static void g_BltTemplate(struct frame *f)
{
	const uint8_t *src = PTR(A(0));
	int srcx = (int16_t)D(0), mod = (int16_t)D(1);
	struct RastPort *rp = RP(A(1));
	int dx = (int16_t)D(2), dy = (int16_t)D(3), w = (int16_t)D(4),
	    h = (int16_t)D(5), x, y;
	struct surf s;

	if (!surf_open(rp, &s))
		return;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int bit = srcx + x;
			plot(&s, rp, dx + x, dy + y,
			     (src[y * mod + (bit >> 3)] >> (7 - (bit & 7))) & 1);
		}
	surf_close(&s);
}

static void g_BltPattern(struct frame *f)
{
	struct RastPort *rp = RP(A(1));
	const uint8_t *mask = PTR(A(0));
	int x0 = (int16_t)D(0), y0 = (int16_t)D(1), x1 = (int16_t)D(2),
	    y1 = (int16_t)D(3), mod = (int16_t)D(4), x, y;
	struct surf s;

	if (!mask) {
		gfx_rectfill(rp, x0, y0, x1, y1, -1);
		return;
	}
	if (!surf_open(rp, &s))
		return;
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++) {
			int bit = x - x0;
			if ((mask[(y - y0) * mod + (bit >> 3)] >> (7 - (bit & 7))) & 1)
				plot(&s, rp, x, y, 1);
		}
	surf_close(&s);
}

static void pixarray(struct frame *f, int write, int line)
{
	struct RastPort *rp = RP(A(0));
	int x0 = (int16_t)D(0), y0 = (int16_t)D(1);
	int x1 = line ? x0 + (int16_t)D(2) - 1 : (int16_t)D(2);
	int y1 = line ? y0 : (int16_t)D(3), x, y, mod;
	uint8_t *arr = PTR(A(2));
	struct surf s;

	RET(0);
	if (!surf_open(rp, &s))
		return;
	mod = line ? 0 : ((x1 - x0 + 1 + 15) & ~15);
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++) {
			int sx = x - s.ox, sy = y - s.oy;
			uint8_t *p = arr + (y - y0) * mod + (x - x0);
			if (sx < s.cx0 || sy < s.cy0 || sx > s.cx1 || sy > s.cy1)
				continue;
			if (write) {
				sset(&s, sx, sy, *p);
				touch(&s, sx, sy, sx, sy);
			} else
				*p = sget(&s, sx, sy);
		}
	surf_close(&s);
	RET((x1 - x0 + 1) * (y1 - y0 + 1));
}

static void g_WritePixelArray8(struct frame *f) { pixarray(f, 1, 0); }
static void g_ReadPixelArray8(struct frame *f) { pixarray(f, 0, 0); }
static void g_WritePixelLine8(struct frame *f) { pixarray(f, 1, 1); }
static void g_ReadPixelLine8(struct frame *f) { pixarray(f, 0, 1); }

static void g_WriteChunkyPixels(struct frame *f)
{
	struct RastPort *rp = RP(A(0));
	int x0 = (int16_t)D(0), y0 = (int16_t)D(1), x1 = (int16_t)D(2),
	    y1 = (int16_t)D(3), bpr = D(4), x, y;
	uint8_t *arr = PTR(A(2));
	struct surf s;

	if (!surf_open(rp, &s))
		return;
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++) {
			int sx = x - s.ox, sy = y - s.oy;
			if (sx < s.cx0 || sy < s.cy0 || sx > s.cx1 || sy > s.cy1)
				continue;
			sset(&s, sx, sy, arr[(y - y0) * bpr + x - x0]);
			touch(&s, sx, sy, sx, sy);
		}
	surf_close(&s);
}

/* ---- colours: the Workbench palette, read only ---- */

static const uint8_t *pal(int n)
{
	return gui_screen.palette[n & 255];
}

static void g_GetRGB4(struct frame *f)
{
	const uint8_t *c = pal(D(0));
	RET((c[0] >> 4) << 8 | (c[1] >> 4) << 4 | c[2] >> 4);
}

static void g_GetRGB32(struct frame *f)
{
	uint32_t *t = PTR(A(1)), i;

	for (i = 0; i < D(1); i++) {
		const uint8_t *c = pal(D(0) + i);
		t[3 * i] = c[0] * 0x01010101u;
		t[3 * i + 1] = c[1] * 0x01010101u;
		t[3 * i + 2] = c[2] * 0x01010101u;
	}
}

static void g_ObtainBestPenA(struct frame *f)
{
	RET(gui_obtain_pen(D(1) >> 24, D(2) >> 24, D(3) >> 24));
}

static void g_ObtainPen(struct frame *f)
{
	if ((int32_t)D(0) != -1) {
		RET(-1);        /* a particular pen: the Workbench keeps them */
		return;
	}
	RET(gui_obtain_pen(D(1) >> 24, D(2) >> 24, D(3) >> 24));
}

static void g_FindColor(struct frame *f)
{
	int best = 0, i, max = (int32_t)D(4) < 0 ? 255 : (int)D(4);
	long bd = 1L << 30;

	for (i = 0; i <= max && i < 256; i++) {
		const uint8_t *c = pal(i);
		long dr = c[0] - (int)(D(1) >> 24), dg = c[1] - (int)(D(2) >> 24),
		     db = c[2] - (int)(D(3) >> 24), d = dr * dr + dg * dg + db * db;
		if (d < bd)
			bd = d, best = i;
	}
	RET(best);
}

static void g_GetColorMap(struct frame *f)
{
	uint8_t *cm = amem_alloc(64);
	*(uint16_t *)(cm + 2) = D(0);
	RET(cm);
}

static void g_GetVPModeID(struct frame *f) { RET(0x8000); }

static void g_WaitTOF(struct frame *f)
{
	gui_flush();
	timer_sleep(20000);
}

static void g_Nop(struct frame *f) { (void)f; }
static void g_Zero(struct frame *f) { RET(0); }
static void g_True(struct frame *f) { RET(1); }

static void g_NewRegion(struct frame *f)
{
	void *r = amem_alloc(16);

	rgn_of(APTR_OF(r));
	RET(r);
}

static void g_DisposeRegion(struct frame *f)
{
	struct rgn **pp, *g;

	for (pp = &rgns; (g = *pp); pp = &g->next)
		if (g->addr == A(0)) {
			*pp = g->next;
			free(g->r);
			free(g);
			break;
		}
	amem_free(PTR(A(0)));
}

static void g_ClearRegion(struct frame *f)
{
	struct rgn *g = rgn_of(A(0));

	g->n = 0;
	rgn_bounds(g);
}

static void g_OrRectRegion(struct frame *f)
{
	struct rgn *g = rgn_of(A(0));
	int16_t *r = PTR(A(1));

	rgn_add(g, r[0], r[1], r[2], r[3]);
	rgn_bounds(g);
	RET(1);
}

static void and_rect(struct rgn *g, int16_t *r)
{
	int i, n = 0;

	for (i = 0; i < g->n; i++) {
		int16_t *q = g->r + 4 * i;
		int x0 = q[0] > r[0] ? q[0] : r[0], y0 = q[1] > r[1] ? q[1] : r[1];
		int x1 = q[2] < r[2] ? q[2] : r[2], y1 = q[3] < r[3] ? q[3] : r[3];
		if (x0 <= x1 && y0 <= y1) {
			int16_t *d = g->r + 4 * n++;
			d[0] = x0, d[1] = y0, d[2] = x1, d[3] = y1;
		}
	}
	g->n = n;
}

static void g_AndRectRegion(struct frame *f)
{
	struct rgn *g = rgn_of(A(0));

	and_rect(g, PTR(A(1)));
	rgn_bounds(g);
}

static void g_ClearRectRegion(struct frame *f)
{
	struct rgn *g = rgn_of(A(0)), old = *g;
	int16_t *r = PTR(A(1));
	int i;

	old.r = malloc(g->n * 8 + 8);
	memcpy(old.r, g->r, g->n * 8);
	g->n = 0;
	for (i = 0; i < old.n; i++) {
		int16_t *q = old.r + 4 * i;
		if (q[2] < r[0] || q[0] > r[2] || q[3] < r[1] || q[1] > r[3]) {
			rgn_add(g, q[0], q[1], q[2], q[3]);
			continue;
		}
		rgn_add(g, q[0], q[1], q[2], r[1] - 1);                 /* above */
		rgn_add(g, q[0], r[3] + 1, q[2], q[3]);                 /* below */
		rgn_add(g, q[0], q[1] > r[1] ? q[1] : r[1], r[0] - 1,
			q[3] < r[3] ? q[3] : r[3]);                     /* left */
		rgn_add(g, r[2] + 1, q[1] > r[1] ? q[1] : r[1], q[2],
			q[3] < r[3] ? q[3] : r[3]);                     /* right */
	}
	free(old.r);
	rgn_bounds(g);
	RET(1);
}

static void g_OrRegionRegion(struct frame *f)
{
	struct rgn *s = rgn_of(A(0)), *d = rgn_of(A(1));
	int i;

	for (i = 0; i < s->n; i++)
		rgn_add(d, s->r[4 * i], s->r[4 * i + 1], s->r[4 * i + 2],
			s->r[4 * i + 3]);
	rgn_bounds(d);
	RET(1);
}

static void g_AndRegionRegion(struct frame *f)
{
	struct rgn *s = rgn_of(A(0)), *d = rgn_of(A(1)), res = { 0 };
	int i;

	for (i = 0; i < s->n; i++) {
		struct rgn t = *d;
		t.r = malloc(d->n * 8 + 8);
		memcpy(t.r, d->r, d->n * 8);
		and_rect(&t, s->r + 4 * i);
		for (int k = 0; k < t.n; k++)
			rgn_add(&res, t.r[4 * k], t.r[4 * k + 1], t.r[4 * k + 2],
				t.r[4 * k + 3]);
		free(t.r);
	}
	free(d->r);
	d->r = res.r;
	d->n = res.n;
	d->cap = res.cap;
	rgn_bounds(d);
	RET(1);
}

/* ---- layers.library: windows are wbde's, so only clip regions are real ---- */

static void l_InstallClipRegion(struct frame *f)
{
	struct lclip *l, **pp;
	uint32_t old = 0;

	for (pp = &lclips; (l = *pp); pp = &l->next)
		if (l->layer == A(0)) {
			old = l->region;
			*pp = l->next;
			free(l);
			break;
		}
	if (A(1)) {
		l = calloc(1, sizeof(*l));
		l->layer = A(0);
		l->region = A(1);
		l->next = lclips;
		lclips = l;
	}
	RET(old);
}

static void l_NewLayerInfo(struct frame *f) { RET(amem_alloc(102)); }

/* a layer on the program's own bitmap: struct Layer with its RastPort */
static void l_CreateLayer(struct frame *f)
{
	uint8_t *l = amem_alloc(160);
	struct RastPort *rp = amem_alloc(sizeof(*rp));
	int16_t *b = (int16_t *)(l + 16);

	gfx_init_rp(rp);
	rp->BitMap = A(1);
	rp->Layer = APTR_OF(l);
	*(uint32_t *)(l + 12) = APTR_OF(rp);
	b[0] = D(0), b[1] = D(1), b[2] = D(2), b[3] = D(3);
	*(uint16_t *)(l + 30) = D(4);           /* Flags */
	RET(l);
}

static void l_DeleteLayer(struct frame *f)
{
	uint8_t *l = PTR(A(1));

	if (l) {
		amem_free(PTR(*(uint32_t *)(l + 12)));
		amem_free(l);
	}
	RET(1);
}

static void l_MoveSizeLayer(struct frame *f)
{
	int16_t *b = (int16_t *)((uint8_t *)PTR(A(0)) + 16);

	b[0] += (int16_t)D(0), b[2] += (int16_t)D(0) + (int16_t)D(2);
	b[1] += (int16_t)D(1), b[3] += (int16_t)D(1) + (int16_t)D(3);
	RET(1);
}

static void l_MoveLayer(struct frame *f)
{
	int16_t *b = (int16_t *)((uint8_t *)PTR(A(1)) + 16);

	b[0] += (int16_t)D(0), b[2] += (int16_t)D(0);
	b[1] += (int16_t)D(1), b[3] += (int16_t)D(1);
	RET(1);
}

static void l_SizeLayer(struct frame *f)
{
	int16_t *b = (int16_t *)((uint8_t *)PTR(A(1)) + 16);

	b[2] += (int16_t)D(0);
	b[3] += (int16_t)D(1);
	RET(1);
}
static void l_Zero(struct frame *f) { RET(0); }
static void l_True(struct frame *f) { RET(1); }

/* layers_lib.fd is not in amitools: offsets from the 3.1 NDK (checked, tools/fdcheck.py) */
static const struct fdent fd_layers[] = {
	{ 30, "InitLayers", "a0" }, { 36, "CreateUpfrontLayer", "a0/a1/d0/d1/d2/d3/d4/a2" },
	{ 42, "CreateBehindLayer", "a0/a1/d0/d1/d2/d3/d4/a2" },
	{ 48, "UpfrontLayer", "a0/a1" }, { 54, "BehindLayer", "a0/a1" },
	{ 60, "MoveLayer", "a0/a1/d0/d1" }, { 66, "SizeLayer", "a0/a1/d0/d1" },
	{ 72, "ScrollLayer", "a0/a1/d0/d1" }, { 78, "BeginUpdate", "a0" },
	{ 84, "EndUpdate", "a0/d0" }, { 90, "DeleteLayer", "a0/a1" },
	{ 96, "LockLayer", "a0/a1" }, { 102, "UnlockLayer", "a0" },
	{ 108, "LockLayers", "a0" }, { 114, "UnlockLayers", "a0" },
	{ 120, "LockLayerInfo", "a0" }, { 126, "SwapBitsRastPortClipRect", "a0/a1" },
	{ 132, "WhichLayer", "a0/d0/d1" }, { 138, "UnlockLayerInfo", "a0" },
	{ 144, "NewLayerInfo", "" }, { 150, "DisposeLayerInfo", "a0" },
	{ 156, "FattenLayerInfo", "a0" }, { 162, "ThinLayerInfo", "a0" },
	{ 168, "MoveLayerInFrontOf", "a0/a1" }, { 174, "InstallClipRegion", "a0/a1" },
	{ 180, "MoveSizeLayer", "a0/d0/d1/d2/d3" },
	{ 186, "CreateUpfrontHookLayer", "a0/a1/d0/d1/d2/d3/d4/a3/a2" },
	{ 192, "CreateBehindHookLayer", "a0/a1/d0/d1/d2/d3/d4/a3/a2" },
	{ 198, "InstallLayerHook", "a0/a1" }, { 204, "InstallLayerInfoHook", "a0/a1" },
	{ 210, "SortLayerCR", "a0/d0/d1" }, { 216, "DoHookClipRects", "a0/a1/a2" },
	{ 0, 0, 0 }
};

static const struct libimpl layers_impl[] = {
	{ "InstallClipRegion", l_InstallClipRegion },
	{ "CreateUpfrontLayer", l_CreateLayer }, { "CreateBehindLayer", l_CreateLayer },
	{ "CreateUpfrontHookLayer", l_CreateLayer },
	{ "CreateBehindHookLayer", l_CreateLayer },
	{ "DeleteLayer", l_DeleteLayer }, { "MoveLayer", l_MoveLayer },
	{ "SizeLayer", l_SizeLayer }, { "MoveSizeLayer", l_MoveSizeLayer },
	{ "UpfrontLayer", l_True }, { "BehindLayer", l_True },
	{ "MoveLayerInFrontOf", l_True }, { "ScrollLayer", l_Zero },
	{ "LockLayer", l_Zero }, { "UnlockLayer", l_Zero },
	{ "LockLayers", l_Zero }, { "UnlockLayers", l_Zero },
	{ "LockLayerInfo", l_Zero }, { "UnlockLayerInfo", l_Zero },
	{ "NewLayerInfo", l_NewLayerInfo }, { "DisposeLayerInfo", l_Zero },
	{ "FattenLayerInfo", l_True }, { "ThinLayerInfo", l_Zero },
	{ "InitLayers", l_Zero }, { "BeginUpdate", l_True }, { "EndUpdate", l_Zero },
	{ "WhichLayer", l_Zero }, { "InstallLayerHook", l_Zero },
	{ "InstallLayerInfoHook", l_Zero }, { "SortLayerCR", l_Zero },
	{ NULL, NULL }
};

struct amilib lib_layers = {
	.name = "layers.library", .version = 40, .revision = 1,
	.fd = fd_layers, .impl = layers_impl, .possize = 8,
};

/* RPTAG_* */
static void g_SetRPAttrsA(struct frame *f)
{
	struct RastPort *rp = RP(A(0));
	struct TagItem *t = PTR(A(1));

	for (; t && t->ti_Tag; t++)
		switch (t->ti_Tag) {
		case 0x80000000: if (t->ti_Data) rp->Font = t->ti_Data; break;
		case 0x80000002: rp->FgPen = t->ti_Data; break;
		case 0x80000003: rp->BgPen = t->ti_Data; break;
		case 0x80000004: rp->DrawMode = t->ti_Data; break;
		case 0x80000005: rp->AOlPen = t->ti_Data; break;
		case 0x80000006: rp->Mask = t->ti_Data; break;
		}
}

static void g_GetRPAttrsA(struct frame *f)
{
	struct RastPort *rp = RP(A(0));
	struct TagItem *t = PTR(A(1));

	for (; t && t->ti_Tag; t++) {
		uint32_t *d = PTR(t->ti_Data);
		switch (t->ti_Tag) {
		case 0x80000000: *d = rp->Font; break;
		case 0x80000002: *d = (uint8_t)rp->FgPen; break;
		case 0x80000003: *d = (uint8_t)rp->BgPen; break;
		case 0x80000004: *d = (uint8_t)rp->DrawMode; break;
		case 0x80000005: *d = (uint8_t)rp->AOlPen; break;
		case 0x80000006: *d = rp->Mask; break;
		case 0x80000007: *d = 255; break;
		}
	}
}

/*
 * The display database: one mode, wbde's screen (640 wide, HIRES_KEY).
 * DisplayInfo, DimensionInfo, MonitorInfo and NameInfo with a QueryHeader
 * each, as GetDisplayInfoData() fills them.
 */
#define HIRES_KEY  0x00008000
#define INVALID_ID 0xffffffff

static void g_BestModeIDA(struct frame *f) { RET(HIRES_KEY); }
static void g_FindDisplayInfo(struct frame *f) { RET(D(0) == HIRES_KEY ? APTR_OF(amem_alloc(16)) : 0); }

static void g_NextDisplayInfo(struct frame *f)
{
	RET(D(0) == INVALID_ID ? HIRES_KEY : INVALID_ID);
}

static void g_GetDisplayInfoData(struct frame *f)
{
	uint8_t *b = PTR(A(1));
	uint32_t size = D(0), tag = D(1);
	int w = gui_screen.width ? gui_screen.width : 640;
	int h = gui_screen.height ? gui_screen.height : 200;
	uint32_t n;

	memset(b, 0, size);
	if (size < 16) {
		RET(0);
		return;
	}
	*(uint32_t *)(b + 0) = tag;
	*(uint32_t *)(b + 4) = HIRES_KEY;
	*(uint32_t *)(b + 8) = 0;               /* SkipID: TAG_SKIP */
	switch (tag) {
	case 0x80000000:                        /* DTAG_DISP */
		n = 56;
		if (size >= 42) {
			*(uint16_t *)(b + 16) = 0;
			*(uint32_t *)(b + 18) = 0x00001000;     /* DIPF_IS_WB */
			*(int16_t *)(b + 22) = 22;
			*(int16_t *)(b + 24) = 44;
			*(uint16_t *)(b + 26) = 70;
			*(uint16_t *)(b + 30) = 4096;
			b[40] = b[41] = b[42] = 8;
		}
		break;
	case 0x80001000:                        /* DTAG_DIMS */
		n = 88;
		if (size >= 66) {
			int16_t *r = (int16_t *)(b + 26);
			int i;
			*(uint16_t *)(b + 16) = 8;
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
	case 0x80002000:                        /* DTAG_MNTR */
		n = 96;
		break;
	case 0x80003000:                        /* DTAG_NAME */
		n = 56;
		if (size >= 48)
			snprintf((char *)b + 16, 32, "Workbench:%dx%d", w, h);
		break;
	default:
		RET(0);
		return;
	}
	*(uint32_t *)(b + 12) = n / 8;          /* Length, in double longs */
	RET(n < size ? n : size);
}

static const struct libimpl gfx_impl[] = {
	{ "BestModeIDA", g_BestModeIDA }, { "FindDisplayInfo", g_FindDisplayInfo },
	{ "NextDisplayInfo", g_NextDisplayInfo },
	{ "GetDisplayInfoData", g_GetDisplayInfoData },
	{ "InitRastPort", g_InitRastPort },
	{ "SetAPen", g_SetAPen }, { "SetBPen", g_SetBPen },
	{ "SetDrMd", g_SetDrMd }, { "SetABPenDrMd", g_SetABPenDrMd },
	{ "GetAPen", g_GetAPen }, { "GetBPen", g_GetBPen },
	{ "GetDrMd", g_GetDrMd }, { "GetOutlinePen", g_GetOutlinePen },
	{ "SetOutlinePen", g_SetOutlinePen }, { "SetWriteMask", g_SetWriteMask },
	{ "SetMaxPen", g_SetMaxPen },
	{ "Move", g_Move }, { "Draw", g_Draw }, { "PolyDraw", g_PolyDraw },
	{ "RectFill", g_RectFill }, { "WritePixel", g_WritePixel },
	{ "ReadPixel", g_ReadPixel }, { "SetRast", g_SetRast },
	{ "EraseRect", g_EraseRect }, { "ScrollRaster", g_ScrollRaster },
	{ "ScrollRasterBF", g_ScrollRaster },
	{ "DrawEllipse", g_DrawEllipse }, { "AreaEllipse", g_AreaEllipse },
	{ "InitArea", g_InitArea }, { "AreaMove", g_AreaMove },
	{ "AreaDraw", g_AreaDraw }, { "AreaEnd", g_AreaEnd },
	{ "InitTmpRas", g_InitTmpRas }, { "Flood", g_Flood },
	{ "OpenFont", g_OpenFont }, { "CloseFont", g_CloseFont },
	{ "SetFont", g_SetFont }, { "AskFont", g_AskFont },
	{ "SetSoftStyle", g_SetSoftStyle }, { "AskSoftStyle", g_AskSoftStyle },
	{ "Text", g_Text }, { "TextLength", g_TextLength },
	{ "TextExtent", g_TextExtent }, { "TextFit", g_TextFit },
	{ "ClearEOL", g_ClearEOL }, { "ClearScreen", g_ClearScreen },
	{ "AddFont", g_Nop }, { "RemFont", g_Nop },
	{ "InitBitMap", g_InitBitMap }, { "AllocRaster", g_AllocRaster },
	{ "FreeRaster", g_FreeRaster }, { "AllocBitMap", g_AllocBitMap },
	{ "FreeBitMap", g_FreeBitMap }, { "GetBitMapAttr", g_GetBitMapAttr },
	{ "BltClear", g_BltClear }, { "BltBitMap", g_BltBitMap },
	{ "BltBitMapRastPort", g_BltBitMapRastPort },
	{ "BltMaskBitMapRastPort", g_BltMaskBitMapRastPort },
	{ "ClipBlit", g_ClipBlit }, { "BltTemplate", g_BltTemplate },
	{ "BltPattern", g_BltPattern },
	{ "WritePixelArray8", g_WritePixelArray8 },
	{ "ReadPixelArray8", g_ReadPixelArray8 },
	{ "WritePixelLine8", g_WritePixelLine8 },
	{ "ReadPixelLine8", g_ReadPixelLine8 },
	{ "WriteChunkyPixels", g_WriteChunkyPixels },
	{ "GetRGB4", g_GetRGB4 }, { "GetRGB32", g_GetRGB32 },
	{ "SetRGB4", g_Nop }, { "SetRGB32", g_Nop }, { "LoadRGB4", g_Nop },
	{ "LoadRGB32", g_Nop }, { "SetRGB4CM", g_Nop }, { "SetRGB32CM", g_Nop },
	{ "ObtainBestPenA", g_ObtainBestPenA }, { "ObtainPen", g_ObtainPen },
	{ "ReleasePen", g_Nop }, { "FindColor", g_FindColor },
	{ "GetColorMap", g_GetColorMap }, { "FreeColorMap", g_Nop },
	{ "GetVPModeID", g_GetVPModeID },
	{ "WaitTOF", g_WaitTOF }, { "WaitBlit", g_Nop },
	{ "OwnBlitter", g_Nop }, { "DisownBlitter", g_Nop },
	{ "VBeamPos", g_Zero }, { "WaitBOVP", g_Nop },
	{ "NewRegion", g_NewRegion }, { "DisposeRegion", g_DisposeRegion },
	{ "ClearRegion", g_ClearRegion }, { "OrRectRegion", g_OrRectRegion },
	{ "AndRectRegion", g_AndRectRegion }, { "XorRectRegion", g_OrRectRegion },
	{ "ClearRectRegion", g_ClearRectRegion },
	{ "OrRegionRegion", g_OrRegionRegion },
	{ "AndRegionRegion", g_AndRegionRegion },
	{ "XorRegionRegion", g_OrRegionRegion },
	{ "LockLayerRom", g_Nop }, { "UnlockLayerRom", g_Nop },
	{ "AttemptLockLayerRom", g_True },
	{ "SetRPAttrsA", g_SetRPAttrsA }, { "GetRPAttrsA", g_GetRPAttrsA },
	{ "ModeNotAvailable", g_Zero },
	{ NULL, NULL }
};

static void gfx_setup(struct Library *b)
{
	GfxBase = b;            /* DefaultFont is set when wbde gives topaz */
}

struct amilib lib_graphics = {
	.name = "graphics.library", .version = 40, .revision = 42,
	.fd = fd_graphics, .impl = gfx_impl,
	.possize = 0x300,
	.init = gfx_setup,
};
