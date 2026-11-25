/*
 * intuition.library on wbde.
 *
 * The Workbench screen is wbde's; a window is a wbde client window whose
 * inner area is a shared buffer of pens (gui.c).  wbde draws the frame and
 * the system gadgets and does all moving, sizing and depth arranging; we
 * keep the Window structure true (RPort, UserPort, borders, sizes, mouse),
 * render and run the application's gadgets, turn wbde's events into
 * IntuiMessages on the UserPort, give wbde the menu strip and draw
 * EasyRequests as windows of our own.  Without wbde, windows cannot be
 * opened (NULL) and requesters are asked on the terminal.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "gui.h"
#include "dos_int.h"

struct Library *IntuitionBase;
static struct MsgPort *imsg_port;       /* replies to our IntuiMessages */

/* IDCMP classes not in wbde_proto.h */
#define IDCMP_SIZEVERIFY   0x00000001
#define IDCMP_GADGETDOWN   0x00000020
#define IDCMP_GADGETUP     0x00000040
#define IDCMP_REQSET       0x00000080
#define IDCMP_MENUVERIFY   0x00002000
#define IDCMP_IDCMPUPDATE  0x00800000

/* window flags */
#define WFLG_SIZEGADGET    0x00000001
#define WFLG_DRAGBAR       0x00000002
#define WFLG_DEPTHGADGET   0x00000004
#define WFLG_CLOSEGADGET   0x00000008
#define WFLG_BACKDROP      0x00000100
#define WFLG_REPORTMOUSE   0x00000200
#define WFLG_GIMMEZEROZERO 0x00000400
#define WFLG_BORDERLESS    0x00000800
#define WFLG_ACTIVATE      0x00001000
#define WFLG_WINDOWACTIVE  0x00002000
#define WFLG_RMBTRAP       0x00010000
#define WFLG_NW_EXTENDED   0x00040000
#define WFLG_HASZOOM       0x20000000

/* gadgets */
#define GFLG_GADGHCOMP     0x0000
#define GFLG_GADGHBOX      0x0001
#define GFLG_GADGHIMAGE    0x0002
#define GFLG_GADGHNONE     0x0003
#define GFLG_GADGIMAGE     0x0004
#define GFLG_RELBOTTOM     0x0008
#define GFLG_RELRIGHT      0x0010
#define GFLG_RELWIDTH      0x0020
#define GFLG_RELHEIGHT     0x0040
#define GFLG_SELECTED      0x0080
#define GFLG_DISABLED      0x0100
#define GACT_RELVERIFY     0x0001
#define GACT_IMMEDIATE     0x0002
#define GACT_FOLLOWMOUSE   0x0008
#define GACT_TOGGLESELECT  0x0100
#define GACT_LONGINT       0x0800
#define GTYP_GTYPEMASK     0x0007
#define GTYP_BOOLGADGET    1
#define GTYP_PROPGADGET    3
#define GTYP_STRGADGET     4
#define GTYP_SYSGADGET     0x8000

/* menus */
#define MENUENABLED  0x0001
#define CHECKIT      0x0001
#define ITEMTEXT     0x0002
#define COMMSEQ      0x0004
#define MENUTOGGLE   0x0008
#define ITEMENABLED  0x0010
#define CHECKED      0x0100
#define MENUNULL     0xffff

/* ---- IntuiMessages ---- */

static void put_imsg(struct awin *a, uint32_t class, uint16_t code,
		     uint16_t qual, APTR iaddr, int mx, int my)
{
	struct Window *w = a->w;
	struct IntuiMessage *m;
	struct timeval tv;
	extern void exec_putmsg(struct MsgPort *p, struct Message *m);

	if (!(w->IDCMPFlags & class) || !w->UserPort)
		return;
	m = amem_alloc(sizeof(*m));
	gettimeofday(&tv, NULL);
	m->ExecMessage.mn_ReplyPort = APTR_OF(imsg_port);
	m->ExecMessage.mn_Length = sizeof(*m);
	m->Class = class;
	m->Code = code;
	m->Qualifier = qual;
	m->IAddress = iaddr ? iaddr : APTR_OF(w);
	m->MouseX = mx;
	m->MouseY = my;
	{
		struct DateStamp ds;
		date_from_unix(tv.tv_sec, &ds);
		m->Seconds = ds.ds_Days * 86400 + ds.ds_Minute * 60 + ds.ds_Tick / 50;
	}
	m->Micros = tv.tv_usec;
	m->IDCMPWindow = APTR_OF(w);
	exec_putmsg(PTR(w->UserPort), &m->ExecMessage);
}

void intui_put_imsg(struct awin *a, uint32_t class, uint16_t code,
		    uint16_t qual, APTR iaddr, int mx, int my)
{
	put_imsg(a, class, code, qual, iaddr, mx, my);
}

/* exec's ReplyMsg comes here for ours: they are simply freed */
int intui_reply(struct Message *m)
{
	if (!imsg_port || m->mn_ReplyPort != APTR_OF(imsg_port))
		return 0;
	amem_free(m);
	return 1;
}

static void free_port_msgs(struct awin *a)
{
	struct MsgPort *p = PTR(a->w->UserPort);
	struct Node *n, *next;

	if (!p)
		return;
	for (n = PTR(p->mp_MsgList.lh_Head); n->ln_Succ; n = next) {
		next = PTR(n->ln_Succ);
		if (((struct IntuiMessage *)n)->IDCMPWindow == APTR_OF(a->w)) {
			((struct Node *)PTR(n->ln_Pred))->ln_Succ = n->ln_Succ;
			((struct Node *)PTR(n->ln_Succ))->ln_Pred = n->ln_Pred;
			amem_free(n);
		}
	}
}

/* ---- gadgets ---- */

static void gbox(struct awin *a, struct Gadget *g, int *x, int *y, int *w, int *h)
{
	struct Window *win = a->w;

	*x = g->LeftEdge + ((g->Flags & GFLG_RELRIGHT) ? win->Width - 1 : 0);
	*y = g->TopEdge + ((g->Flags & GFLG_RELBOTTOM) ? win->Height - 1 : 0);
	*w = g->Width + ((g->Flags & GFLG_RELWIDTH) ? win->Width : 0);
	*h = g->Height + ((g->Flags & GFLG_RELHEIGHT) ? win->Height : 0);
}

static void ghost(struct RastPort *rp, int x, int y, int w, int h)
{
	static const uint16_t pat[2] = { 0x8888, 0x2222 };
	uint16_t *p = amem_alloc(4);
	int8_t fg = rp->FgPen, dm = rp->DrawMode;
	APTR ap = rp->AreaPtrn;
	int8_t sz = rp->AreaPtSz;

	memcpy(p, pat, 4);
	rp->AreaPtrn = APTR_OF(p);
	rp->AreaPtSz = 1;
	rp->FgPen = gui_pens[WBP_BLOCKPEN];
	rp->DrawMode = JAM1;
	gfx_rectfill(rp, x, y, x + w - 1, y + h - 1, -1);
	rp->AreaPtrn = ap;
	rp->AreaPtSz = sz;
	rp->FgPen = fg;
	rp->DrawMode = dm;
	amem_free(p);
}

static void render_prop(struct awin *a, struct Gadget *g, int x, int y, int w, int h)
{
	struct PropInfo *pi = PTR(g->SpecialInfo);
	struct RastPort *rp = a->rp;
	int kx = x + 2, ky = y + 2, kw = w - 4, kh = h - 4, bw, bh;

	if (!pi)
		return;
	if (!(pi->Flags & 0x08))                /* not PROPBORDERLESS */
		gfx_bevel(rp, x, y, w, h, 1);
	else
		kx = x, ky = y, kw = w, kh = h;
	gfx_rectfill(rp, kx, ky, kx + kw - 1, ky + kh - 1, gui_pens[WBP_BACKGROUNDPEN]);
	bw = (pi->Flags & 2) ? kw * pi->HorizBody / 0xffff : kw;
	bh = (pi->Flags & 4) ? kh * pi->VertBody / 0xffff : kh;
	if (bw < 4) bw = kw < 4 ? kw : 4;
	if (bh < 4) bh = kh < 4 ? kh : 4;
	if (pi->Flags & 2)
		kx += (kw - bw) * pi->HorizPot / 0xffff;
	if (pi->Flags & 4)
		ky += (kh - bh) * pi->VertPot / 0xffff;
	pi->CWidth = kw;
	pi->CHeight = kh;
	if (pi->Flags & 1) {                    /* AUTOKNOB */
		gfx_rectfill(rp, kx, ky, kx + bw - 1, ky + bh - 1,
			     gui_pens[WBP_FILLPEN]);
		gfx_bevel(rp, kx, ky, bw, bh, 0);
	} else if (g->GadgetRender)
		gfx_drawimage(rp, PTR(g->GadgetRender), kx, ky);
}

static void render_string(struct awin *a, struct Gadget *g, int x, int y, int w, int h)
{
	struct StringInfo *si = PTR(g->SpecialInfo);
	struct RastPort *rp = a->rp;
	const char *buf;
	int n, cw, fits, pos, top;

	if (!si || !si->Buffer)
		return;
	buf = PTR(si->Buffer);
	n = strlen(buf);
	cw = gui_topaz ? gui_topaz->tf_XSize : 8;
	fits = w / cw;
	if (si->BufferPos < si->DispPos)
		si->DispPos = si->BufferPos;
	if (si->BufferPos >= si->DispPos + fits)
		si->DispPos = si->BufferPos - fits + 1;
	if (si->DispPos < 0)
		si->DispPos = 0;
	si->NumChars = n;
	gfx_rectfill(rp, x, y, x + w - 1, y + h - 1, gui_pens[WBP_BACKGROUNDPEN]);
	top = y + (h - (gui_topaz ? gui_topaz->tf_YSize : 8)) / 2;
	rp->FgPen = gui_pens[WBP_TEXTPEN];
	rp->BgPen = gui_pens[WBP_BACKGROUNDPEN];
	rp->DrawMode = JAM2;
	rp->cp_x = x;
	rp->cp_y = top + (gui_topaz ? gui_topaz->tf_Baseline : 6);
	pos = si->DispPos < n ? si->DispPos : n;
	gfx_text(rp, buf + pos, n - pos < fits ? n - pos : fits);
	if (a->strgad == g) {                   /* the cursor */
		int cx = x + (si->BufferPos - si->DispPos) * cw;
		gfx_complement(rp, cx, top, cx + cw - 1,
			       top + (gui_topaz ? gui_topaz->tf_YSize : 8) - 1);
	}
}

static void render_one(struct awin *a, struct Gadget *g)
{
	struct RastPort *rp = a->rp;
	int x, y, w, h, type = g->GadgetType & GTYP_GTYPEMASK;
	int sel = (g->Flags & GFLG_SELECTED) != 0;

	if (g->GadgetType & GTYP_SYSGADGET)
		return;
	if (gt_render(a, g))
		return;
	gbox(a, g, &x, &y, &w, &h);
	if (type == GTYP_PROPGADGET) {
		render_prop(a, g, x, y, w, h);
		return;
	}
	if (type == GTYP_STRGADGET) {
		if (g->GadgetRender)
			gfx_drawborder(rp, PTR(g->GadgetRender), x, y);
		render_string(a, g, x, y, w, h);
		if (g->GadgetText)
			gfx_printitext(rp, PTR(g->GadgetText), x, y);
		return;
	}
	{
		uint32_t r = (sel && (g->Flags & 3) == GFLG_GADGHIMAGE &&
			      g->SelectRender) ? g->SelectRender : g->GadgetRender;
		if (r && (g->Flags & GFLG_GADGIMAGE))
			gfx_drawimage(rp, PTR(r), x, y);
		else if (r)
			gfx_drawborder(rp, PTR(r), x, y);
	}
	if (g->GadgetText && !(g->Flags & 0x3000))     /* LABELITEXT */
		gfx_printitext(rp, PTR(g->GadgetText), x, y);
	if (sel && (g->Flags & 3) == GFLG_GADGHCOMP)
		gfx_complement(rp, x, y, x + w - 1, y + h - 1);
	if (sel && (g->Flags & 3) == GFLG_GADGHBOX) {
		gfx_complement(rp, x - 4, y - 2, x + w + 3, y - 1);
		gfx_complement(rp, x - 4, y + h, x + w + 3, y + h + 1);
	}
	if (g->Flags & GFLG_DISABLED)
		ghost(rp, x, y, w, h);
}

void intui_render_gadgets(struct awin *a, struct Gadget *g, int n)
{
	for (; g && n; g = PTR(g->NextGadget), n--)
		render_one(a, g);
}

static struct Gadget *hit(struct awin *a, int x, int y)
{
	struct Gadget *g;

	for (g = PTR(a->w->FirstGadget); g; g = PTR(g->NextGadget)) {
		int gx, gy, gw, gh;
		if (g->GadgetType & GTYP_SYSGADGET)
			continue;
		gbox(a, g, &gx, &gy, &gw, &gh);
		if (x >= gx && y >= gy && x < gx + gw && y < gy + gh &&
		    !(g->Flags & GFLG_DISABLED))
			return g;
	}
	return NULL;
}

static int inside(struct awin *a, struct Gadget *g, int x, int y)
{
	int gx, gy, gw, gh;

	gbox(a, g, &gx, &gy, &gw, &gh);
	return x >= gx && y >= gy && x < gx + gw && y < gy + gh;
}

/* a click on a proportional gadget: page, or move the knob to the click */
static void prop_set(struct awin *a, struct Gadget *g, int x, int y)
{
	struct PropInfo *pi = PTR(g->SpecialInfo);
	int gx, gy, gw, gh;

	if (!pi)
		return;
	gbox(a, g, &gx, &gy, &gw, &gh);
	if ((pi->Flags & 2) && gw > 4) {
		long v = (long)(x - gx - 2) * 0xffff / (gw - 4);
		pi->HorizPot = v < 0 ? 0 : v > 0xffff ? 0xffff : v;
	}
	if ((pi->Flags & 4) && gh > 4) {
		long v = (long)(y - gy - 2) * 0xffff / (gh - 4);
		pi->VertPot = v < 0 ? 0 : v > 0xffff ? 0xffff : v;
	}
	render_one(a, g);
}

static void end_string(struct awin *a, int report, int code)
{
	struct Gadget *g = a->strgad;

	if (!g)
		return;
	a->strgad = NULL;
	if (g->Activation & GACT_LONGINT) {
		struct StringInfo *si = PTR(g->SpecialInfo);
		si->LongInt = strtol(PTR(si->Buffer), NULL, 10);
	}
	render_one(a, g);
	if (report && (g->Activation & GACT_RELVERIFY))
		put_imsg(a, IDCMP_GADGETUP, code, 0, APTR_OF(g), a->w->MouseX,
			 a->w->MouseY);
}

static int string_key(struct awin *a, int ch, int raw)
{
	struct Gadget *g = a->strgad;
	struct StringInfo *si = PTR(g->SpecialInfo);
	char *buf = PTR(si->Buffer);
	int n = strlen(buf);

	if (raw == 0x44 || raw == 0x43 || ch == '\r') {         /* Return */
		end_string(a, 1, 0);
		return 1;
	}
	if (raw == 0x42 || ch == '\t') {                        /* Tab */
		end_string(a, 1, 9);
		return 1;
	}
	if (raw == 0x4f && si->BufferPos > 0)
		si->BufferPos--;
	else if (raw == 0x4e && si->BufferPos < n)
		si->BufferPos++;
	else if ((raw == 0x41 || ch == 8) && si->BufferPos > 0) {
		memmove(buf + si->BufferPos - 1, buf + si->BufferPos,
			n - si->BufferPos + 1);
		si->BufferPos--;
	} else if ((raw == 0x46 || ch == 127) && si->BufferPos < n)
		memmove(buf + si->BufferPos, buf + si->BufferPos + 1,
			n - si->BufferPos);
	else if (ch >= 32 && ch != 127 && n < si->MaxChars - 1) {
		if ((g->Activation & GACT_LONGINT) && !(ch >= '0' && ch <= '9') &&
		    ch != '-')
			return 1;
		memmove(buf + si->BufferPos + 1, buf + si->BufferPos,
			n - si->BufferPos + 1);
		buf[si->BufferPos++] = ch;
	} else if (raw < 0 || ch == 0)
		return 0;
	render_one(a, g);
	return 1;
}

/* ---- events from wbde ---- */

static void deliver_raw(struct awin *a)
{
	if (!a->raw_pending)
		return;
	a->raw_pending = 0;
	put_imsg(a, IDCMP_RAWKEY, a->raw.code, a->raw.qualifier, 0,
		 a->raw.mousex + a->bl, a->raw.mousey + a->bt);
}

void intui_easy_event(struct awin *a, const struct wbp_event *e);

void intui_event(struct awin *a, const struct wbp_event *e)
{
	struct Window *w = a->w;
	int mx = e->mousex + a->bl, my = e->mousey + a->bt;

	if (a->easy) {
		intui_easy_event(a, e);
		return;
	}
	if (e->class != IDCMP_VANILLAKEY)
		deliver_raw(a);
	if (e->class & (IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE)) {
		w->MouseX = mx;
		w->MouseY = my;
		w->GZZMouseX = e->mousex;
		w->GZZMouseY = e->mousey;
	}
	switch (e->class) {
	case IDCMP_MOUSEBUTTONS:
		if (e->code == SELECTDOWN) {
			struct Gadget *g = hit(a, mx, my);
			if (a->strgad && a->strgad != g)
				end_string(a, 1, 0);
			if (g) {
				int type = g->GadgetType & GTYP_GTYPEMASK;
				a->agad = g;
				a->agad_in = 1;
				if (gt_handle_down(a, g, mx, my))
					return;
				if (type == GTYP_STRGADGET) {
					struct StringInfo *si = PTR(g->SpecialInfo);
					a->strgad = g;
					a->agad = NULL;
					if (si) {
						int cw = gui_topaz ? gui_topaz->tf_XSize : 8;
						int gx, gy, gw, gh, n = strlen(PTR(si->Buffer));
						gbox(a, g, &gx, &gy, &gw, &gh);
						si->BufferPos = si->DispPos + (mx - gx) / cw;
						if (si->BufferPos > n)
							si->BufferPos = n;
					}
					render_one(a, g);
				} else if (type == GTYP_PROPGADGET) {
					g->Flags |= GFLG_SELECTED;
					prop_set(a, g, mx, my);
				} else {
					if (g->Activation & GACT_TOGGLESELECT)
						g->Flags ^= GFLG_SELECTED;
					else
						g->Flags |= GFLG_SELECTED;
					render_one(a, g);
				}
				if (g->Activation & GACT_IMMEDIATE)
					put_imsg(a, IDCMP_GADGETDOWN, 0, e->qualifier,
						 APTR_OF(g), mx, my);
				return;
			}
		} else if (e->code == SELECTUP && a->agad) {
			struct Gadget *g = a->agad;
			int in = inside(a, g, mx, my);
			a->agad = NULL;
			if (gt_handle_up(a, g, mx, my, in))
				return;
			if ((g->GadgetType & GTYP_GTYPEMASK) == GTYP_PROPGADGET) {
				g->Flags &= ~GFLG_SELECTED;
				render_one(a, g);
				if (g->Activation & GACT_RELVERIFY)
					put_imsg(a, IDCMP_GADGETUP, 0, e->qualifier,
						 APTR_OF(g), mx, my);
				return;
			}
			if (!(g->Activation & GACT_TOGGLESELECT)) {
				g->Flags &= ~GFLG_SELECTED;
				render_one(a, g);
			}
			if (in && (g->Activation & GACT_RELVERIFY))
				put_imsg(a, IDCMP_GADGETUP, 0, e->qualifier,
					 APTR_OF(g), mx, my);
			return;
		}
		put_imsg(a, IDCMP_MOUSEBUTTONS, e->code, e->qualifier, 0, mx, my);
		return;
	case IDCMP_MOUSEMOVE:
		if (a->agad) {
			struct Gadget *g = a->agad;
			int in = inside(a, g, mx, my);
			if (gt_handle_move(a, g, mx, my))
				return;
			if ((g->GadgetType & GTYP_GTYPEMASK) == GTYP_PROPGADGET)
				prop_set(a, g, mx, my);
			else if (in != a->agad_in &&
				 !(g->Activation & GACT_TOGGLESELECT)) {
				a->agad_in = in;
				if (in)
					g->Flags |= GFLG_SELECTED;
				else
					g->Flags &= ~GFLG_SELECTED;
				render_one(a, g);
			}
			if (g->Activation & GACT_FOLLOWMOUSE)
				put_imsg(a, IDCMP_MOUSEMOVE, 0, e->qualifier,
					 APTR_OF(g), mx, my);
			return;
		}
		if (w->Flags & WFLG_REPORTMOUSE)
			put_imsg(a, IDCMP_MOUSEMOVE, 0, e->qualifier, 0, mx, my);
		return;
	case IDCMP_RAWKEY:
		if (a->strgad && !(e->code & 0x80) && string_key(a, 0, e->code))
			return;
		if (a->strgad && (e->code & 0x80))
			return;
		if (a->agad && gt_key(a, a->agad, 0, e->code, e->qualifier))
			return;
		if (!(e->code & 0x80) && (w->IDCMPFlags & IDCMP_VANILLAKEY)) {
			a->raw = *e;            /* a VANILLAKEY may follow */
			a->raw_pending = 1;
			return;
		}
		put_imsg(a, IDCMP_RAWKEY, e->code, e->qualifier, 0, mx, my);
		return;
	case IDCMP_VANILLAKEY:
		a->raw_pending = 0;
		if (a->strgad)
			return;         /* the RAWKEY did the editing */
		if (gt_key(a, NULL, e->code, -1, e->qualifier))
			return;
		put_imsg(a, IDCMP_VANILLAKEY, e->code, e->qualifier, 0, mx, my);
		return;
	case IDCMP_MENUPICK: {
		extern void menu_picked(struct awin *a, uint16_t code);
		menu_picked(a, e->code);
		put_imsg(a, IDCMP_MENUPICK, e->code, e->qualifier, 0, w->MouseX,
			 w->MouseY);
		return;
	}
	case IDCMP_ACTIVEWINDOW:
		w->Flags |= WFLG_WINDOWACTIVE;
		break;
	case IDCMP_INACTIVEWINDOW:
		w->Flags &= ~WFLG_WINDOWACTIVE;
		if (a->strgad)
			end_string(a, 1, 0);
		break;
	}
	put_imsg(a, e->class, e->code, e->qualifier, 0, mx, my);
}

/* a new buffer after a resize: NEWSIZE follows as an event */
void intui_resized(struct awin *a)
{
	struct RastPort *rp = a->rp;

	if (!rp)
		return;
	/* the area that was not there before is the background pen */
	intui_render_gadgets(a, PTR(a->w->FirstGadget), -1);
	gui_damage(a, 0, 0, a->iw - 1, a->ih - 1);
	put_imsg(a, IDCMP_REFRESHWINDOW, 0, 0, 0, 0, 0);
}

/* the tick that INTUITICKS wants, ten a second, from exec's Wait() */
void intui_ticks(void)
{
	struct awin *a;

	for (a = gui_wins; a; a = a->next)
		if ((a->w->Flags & WFLG_WINDOWACTIVE) &&
		    (a->w->IDCMPFlags & IDCMP_INTUITICKS)) {
			struct MsgPort *p = PTR(a->w->UserPort);
			struct Node *n;
			int queued = 0;
			if (!p)
				continue;
			for (n = PTR(p->mp_MsgList.lh_Head); n->ln_Succ;
			     n = PTR(n->ln_Succ))
				if (((struct IntuiMessage *)n)->Class ==
				    IDCMP_INTUITICKS)
					queued = 1;
			if (!queued)
				put_imsg(a, IDCMP_INTUITICKS, 0, 0, 0, a->w->MouseX,
					 a->w->MouseY);
		}
}

int intui_wants_ticks(void)
{
	struct awin *a;

	for (a = gui_wins; a; a = a->next)
		if (a->w->IDCMPFlags & IDCMP_INTUITICKS)
			return 1;
	return 0;
}

/* ---- windows ---- */

static struct MsgPort *new_port(void)
{
	struct MsgPort *p = amem_alloc(sizeof(*p));
	extern void exec_newlist(void *l);
	struct Task *t = &me->pr_Task;
	int n;

	for (n = 31; n >= 16 && (t->tc_SigAlloc & (1u << n)); n--)
		;
	if (n < 16)
		n = 16;
	t->tc_SigAlloc |= 1u << n;
	p->mp_Node.ln_Type = NT_MSGPORT;
	p->mp_SigBit = n;
	p->mp_SigTask = APTR_OF(me);
	exec_newlist(&p->mp_MsgList);
	return p;
}

struct wspec {
	int left, top, width, height, iw, ih;
	int minw, minh, maxw, maxh;
	uint32_t flags, idcmp;
	APTR title, gadgets, screentitle;
	int zoom;
};

static void read_tags(struct wspec *s, struct TagItem *t)
{
	extern struct TagItem *util_next_tag(struct TagItem **tp);
	struct TagItem *x;

	while ((x = util_next_tag(&t))) {
		uint32_t d = x->ti_Data;
		switch (x->ti_Tag - 0x80000063) {
		case 1: s->left = (int16_t)d; break;
		case 2: s->top = (int16_t)d; break;
		case 3: s->width = d; break;
		case 4: s->height = d; break;
		case 7: s->idcmp = d; break;
		case 8: s->flags = d; break;
		case 9: s->gadgets = d; break;
		case 11: s->title = d; break;
		case 12: s->screentitle = d; break;
		case 15: s->minw = d; break;
		case 16: s->minh = d; break;
		case 17: s->maxw = d; break;
		case 18: s->maxh = d; break;
		case 19: s->iw = d; break;
		case 20: s->ih = d; break;
		case 26: s->zoom = 1; break;
#define FL(bit) s->flags = d ? s->flags | (bit) : s->flags & ~(bit)
		case 30: FL(WFLG_SIZEGADGET); break;
		case 31: FL(WFLG_DRAGBAR); break;
		case 32: FL(WFLG_DEPTHGADGET); break;
		case 33: FL(WFLG_CLOSEGADGET); break;
		case 34: FL(WFLG_BACKDROP); break;
		case 35: FL(WFLG_REPORTMOUSE); break;
		case 37: FL(WFLG_BORDERLESS); break;
		case 38: FL(WFLG_ACTIVATE); break;
		case 39: FL(WFLG_RMBTRAP); break;
		case 46: FL(WFLG_GIMMEZEROZERO); break;
#undef FL
		}
	}
}

static struct Window *open_window(struct wspec *s)
{
	struct wbp_open o;
	struct awin *a;
	struct Window *w;
	struct RastPort *rp;
	int sizing = (s->flags & WFLG_SIZEGADGET) != 0;
	int bl = 4, br = sizing ? 18 : 4, bb = sizing ? 10 : 2, bt;
	uint8_t *layer;

	if (gui_connect()) {
		af_warn("%s: no wbde to open a window on (WBDE_SOCKET)",
			af_progname);
		return NULL;
	}
	bt = gui_screen.wbortop;
	if (s->flags & WFLG_BORDERLESS)
		bl = br = bt = bb = 0;
	memset(&o, 0, sizeof(o));
	o.left = s->left;
	o.top = s->top;
	o.width = s->iw ? s->iw : (s->width ? s->width : gui_screen.width - s->left) - bl - br;
	o.height = s->ih ? s->ih : (s->height ? s->height : gui_screen.height - s->top) - bt - bb;
	if ((int16_t)o.width <= 0)
		o.width = 1;
	if ((int16_t)o.height <= 0)
		o.height = 1;
	/* limits matter only for sizing, never for the size it opens at */
	if (sizing) {
		if (s->minw > bl + br)
			o.minwidth = s->minw - bl - br;
		if (s->minh > bt + bb)
			o.minheight = s->minh - bt - bb;
		if (s->maxw > 0 && s->maxw < 0xffff)
			o.maxwidth = s->maxw - bl - br;
		if (s->maxh > 0 && s->maxh < 0xffff)
			o.maxheight = s->maxh - bt - bb;
		if (o.minwidth > o.width) o.minwidth = o.width;
		if (o.minheight > o.height) o.minheight = o.height;
		if (o.maxwidth && o.maxwidth < o.width) o.maxwidth = o.width;
		if (o.maxheight && o.maxheight < o.height) o.maxheight = o.height;
	}
	if (s->flags & WFLG_CLOSEGADGET) o.flags |= WBP_WF_CLOSE;
	if (s->flags & WFLG_DEPTHGADGET) o.flags |= WBP_WF_DEPTH;
	if (s->flags & WFLG_SIZEGADGET) o.flags |= WBP_WF_SIZE;
	if (s->flags & WFLG_DRAGBAR) o.flags |= WBP_WF_DRAG;
	if (s->flags & WFLG_BORDERLESS) o.flags |= WBP_WF_BORDERLESS;
	if (s->flags & WFLG_BACKDROP) o.flags |= WBP_WF_BACKDROP;
	if (s->flags & WFLG_RMBTRAP) o.flags |= WBP_WF_RMBTRAP;
	if (s->flags & WFLG_ACTIVATE) o.flags |= WBP_WF_ACTIVATE;
	if (s->flags & WFLG_REPORTMOUSE) o.flags |= WBP_WF_REPORTMOUSE;
	if (s->zoom || (s->flags & WFLG_HASZOOM)) o.flags |= WBP_WF_ZOOM;
	o.idcmp = IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY |
		  IDCMP_VANILLAKEY | IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE |
		  IDCMP_MENUPICK | IDCMP_ACTIVEWINDOW | IDCMP_INACTIVEWINDOW;
	if (s->title)
		snprintf(o.title, sizeof(o.title), "%s", (char *)PTR(s->title));
	if (af_trace)
		fprintf(stderr, "amigafmt: window at %d,%d size %d,%d inner %d,%d "
			"flags $%x min %d,%d max %d,%d -> inner %d,%d\n", s->left, s->top, s->width,
			s->height, s->iw, s->ih, s->flags, s->minw, s->minh, s->maxw, s->maxh, o.width, o.height);
	a = gui_open(&o);
	if (!a)
		return NULL;
	w = a->w;
	rp = amem_alloc(sizeof(*rp));
	gfx_init_rp(rp);
	rp->FgPen = 1;
	a->rp = rp;
	a->bm = amem_alloc(sizeof(struct BitMap));
	*a->bm = wb_screen->BitMap;
	rp->BitMap = APTR_OF(a->bm);
	layer = amem_alloc(160);                /* a Layer: rp, bounds, Window */
	*(uint32_t *)(layer + 12) = APTR_OF(rp);
	*(uint32_t *)(layer + 40) = APTR_OF(w);
	rp->Layer = APTR_OF(layer);
	w->RPort = APTR_OF(rp);
	w->WLayer = APTR_OF(layer);
	w->WScreen = APTR_OF(wb_screen);
	w->IFont = APTR_OF(gui_topaz);
	w->Flags = s->flags;
	if (s->flags & WFLG_ACTIVATE)
		w->Flags |= WFLG_WINDOWACTIVE;
	w->Title = s->title;
	w->ScreenTitle = s->screentitle;
	w->MinWidth = s->minw;
	w->MinHeight = s->minh;
	w->MaxWidth = s->maxw;
	w->MaxHeight = s->maxh;
	w->DetailPen = gui_pens[WBP_DETAILPEN];
	w->BlockPen = gui_pens[WBP_BLOCKPEN];
	w->IDCMPFlags = s->idcmp;
	if (s->idcmp)
		w->UserPort = APTR_OF(new_port());
	w->FirstGadget = s->gadgets;
	w->NextWindow = wb_screen->FirstWindow;
	wb_screen->FirstWindow = APTR_OF(w);
	intui_render_gadgets(a, PTR(w->FirstGadget), -1);
	return w;
}

static void i_OpenWindow(struct frame *f)
{
	struct NewWindow *nw = PTR(A(0));
	struct wspec s;

	memset(&s, 0, sizeof(s));
	s.left = nw->LeftEdge;
	s.top = nw->TopEdge;
	s.width = nw->Width;
	s.height = nw->Height;
	s.idcmp = nw->IDCMPFlags;
	s.flags = nw->Flags;
	s.gadgets = nw->FirstGadget;
	s.title = nw->Title;
	s.minw = nw->MinWidth;
	s.minh = nw->MinHeight;
	s.maxw = nw->MaxWidth;
	s.maxh = nw->MaxHeight;
	if ((nw->Flags & WFLG_NW_EXTENDED) && nw->Extension)
		read_tags(&s, PTR(nw->Extension));
	RET(open_window(&s));
}

static void i_OpenWindowTagList(struct frame *f)
{
	struct NewWindow *nw = PTR(A(0));
	struct wspec s;

	memset(&s, 0, sizeof(s));
	s.maxw = s.maxh = 0xffff;
	if (nw) {
		s.left = nw->LeftEdge;
		s.top = nw->TopEdge;
		s.width = nw->Width;
		s.height = nw->Height;
		s.idcmp = nw->IDCMPFlags;
		s.flags = nw->Flags;
		s.gadgets = nw->FirstGadget;
		s.title = nw->Title;
		s.minw = nw->MinWidth;
		s.minh = nw->MinHeight;
		s.maxw = nw->MaxWidth;
		s.maxh = nw->MaxHeight;
	}
	read_tags(&s, PTR(A(1)));
	RET(open_window(&s));
}

static void i_CloseWindow(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct Window *w;
	uint32_t *pp;

	if (!a)
		return;
	w = a->w;
	free_port_msgs(a);
	for (pp = &wb_screen->FirstWindow; *pp; pp = &((struct Window *)PTR(*pp))->NextWindow)
		if (*pp == APTR_OF(w)) {
			*pp = w->NextWindow;
			break;
		}
	gui_flush();
	gui_close(a);
}

static void i_ModifyIDCMP(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct Window *w = PTR(A(0));

	RET(1);
	if (!a)
		return;
	if (D(0) && !w->UserPort)
		w->UserPort = APTR_OF(new_port());
	else if (!D(0) && w->UserPort) {
		free_port_msgs(a);
		/* Intuition frees the port only if it made it; we cannot tell
		   a program's shared port apart, so it is left alone */
		w->UserPort = 0;
	}
	w->IDCMPFlags = D(0);
}

static void i_SetWindowTitles(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct Window *w = PTR(A(0));

	if (!a)
		return;
	if (A(1) != 0xffffffff) {
		const char *t = A(1) ? PTR(A(1)) : "";
		w->Title = A(1);
		gui_send(WBP_SET_TITLE, a->id, t, strlen(t));
	}
	if (A(2) != 0xffffffff)
		w->ScreenTitle = A(2);
}

static void i_ActivateWindow(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	if (a)
		gui_send(WBP_ACTIVATE, a->id, NULL, 0);
}

static void i_WindowToFront(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	if (a)
		gui_send(WBP_TO_FRONT, a->id, NULL, 0);
}

static void i_WindowToBack(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	if (a)
		gui_send(WBP_TO_BACK, a->id, NULL, 0);
}

static void move_size(struct awin *a, int x, int y, int w, int h)
{
	struct wbp_rect r = { x, y, w - a->bl - a->br, h - a->bt - a->bb };

	if ((int16_t)r.w < 1) r.w = 1;
	if ((int16_t)r.h < 1) r.h = 1;
	gui_send(WBP_MOVE_SIZE, a->id, &r, sizeof(r));
}

static void i_MoveWindow(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct Window *w = PTR(A(0));
	if (a)
		move_size(a, w->LeftEdge + (int16_t)D(0), w->TopEdge + (int16_t)D(1),
			  w->Width, w->Height);
}

static void i_SizeWindow(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct Window *w = PTR(A(0));
	if (a)
		move_size(a, w->LeftEdge, w->TopEdge, w->Width + (int16_t)D(0),
			  w->Height + (int16_t)D(1));
}

static void i_ChangeWindowBox(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	if (a)
		move_size(a, (int16_t)D(0), (int16_t)D(1), (int16_t)D(2), (int16_t)D(3));
}

static void i_WindowLimits(struct frame *f)
{
	struct Window *w = PTR(A(0));

	if (D(0)) w->MinWidth = D(0);
	if (D(1)) w->MinHeight = D(1);
	if (D(2)) w->MaxWidth = D(2);
	if (D(3)) w->MaxHeight = D(3);
	RET(1);
}

static void i_RefreshWindowFrame(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	if (a)
		intui_render_gadgets(a, PTR(a->w->FirstGadget), -1);
}

/* ---- gadgets ---- */

static void i_AddGList(struct frame *f)
{
	struct Window *w = PTR(A(0));
	struct Gadget *g = PTR(A(1)), *last = g;
	int pos = (int16_t)D(0), n = (int16_t)D(1), i;
	uint32_t *pp = &w->FirstGadget;

	if (!g) {
		RET(-1);
		return;
	}
	for (i = 1; (n < 0 || i < n) && last->NextGadget; i++)
		last = PTR(last->NextGadget);
	for (i = 0; *pp && (pos < 0 || i < pos); i++)
		pp = &((struct Gadget *)PTR(*pp))->NextGadget;
	last->NextGadget = *pp;
	*pp = A(1);
	RET(i);
}

static void i_AddGadget(struct frame *f)
{
	D(1) = 1;
	i_AddGList(f);
}

static void i_RemoveGList(struct frame *f)
{
	struct Window *w = PTR(A(0));
	struct Gadget *g = PTR(A(1)), *last = g;
	int n = (int16_t)D(0), i;
	uint32_t *pp = &w->FirstGadget;

	RET(-1);
	if (!g)
		return;
	for (i = 1; (n < 0 || i < n) && last->NextGadget; i++)
		last = PTR(last->NextGadget);
	for (i = 0; *pp; i++, pp = &((struct Gadget *)PTR(*pp))->NextGadget)
		if (*pp == A(1)) {
			struct awin *a = gui_by_window(A(0));
			*pp = last->NextGadget;
			last->NextGadget = 0;
			if (a && (a->agad == g || a->strgad == g))
				a->agad = a->strgad = NULL;
			RET(i);
			return;
		}
}

static void i_RemoveGadget(struct frame *f)
{
	D(0) = 1;
	i_RemoveGList(f);
}

static void i_RefreshGList(struct frame *f)
{
	struct awin *a = gui_by_window(A(1));

	if (a)
		intui_render_gadgets(a, PTR(A(0)), (int16_t)D(0));
}

static void i_RefreshGadgets(struct frame *f)
{
	D(0) = -1;
	i_RefreshGList(f);
}

static void i_OnGadget(struct frame *f)
{
	struct Gadget *g = PTR(A(0));
	struct awin *a = gui_by_window(A(1));

	g->Flags &= ~GFLG_DISABLED;
	if (a)
		intui_render_gadgets(a, g, 1);
}

static void i_OffGadget(struct frame *f)
{
	struct Gadget *g = PTR(A(0));
	struct awin *a = gui_by_window(A(1));

	g->Flags |= GFLG_DISABLED;
	if (a)
		intui_render_gadgets(a, g, 1);
}

static void i_ActivateGadget(struct frame *f)
{
	struct Gadget *g = PTR(A(0));
	struct awin *a = gui_by_window(A(1));

	RET(0);
	if (!a || (g->GadgetType & GTYP_GTYPEMASK) != GTYP_STRGADGET)
		return;
	if (a->strgad)
		end_string(a, 0, 0);
	a->strgad = g;
	render_one(a, g);
	RET(1);
}

static void i_NewModifyProp(struct frame *f)
{
	struct Gadget *g = PTR(A(0));
	struct PropInfo *pi = PTR(g->SpecialInfo);
	struct awin *a = gui_by_window(A(1));

	if (!pi)
		return;
	pi->Flags = D(0);
	pi->HorizPot = D(1);
	pi->VertPot = D(2);
	pi->HorizBody = D(3);
	pi->VertBody = D(4);
	if (a)
		render_one(a, g);
}

static void i_ModifyProp(struct frame *f)
{
	i_NewModifyProp(f);
}

/* ---- drawing helpers that live in intuition ---- */

static void i_DrawBorder(struct frame *f)
{
	gfx_drawborder(PTR(A(0)), PTR(A(1)), (int16_t)D(0), (int16_t)D(1));
}

static void i_DrawImage(struct frame *f)
{
	gfx_drawimage(PTR(A(0)), PTR(A(1)), (int16_t)D(0), (int16_t)D(1));
}

static void i_DrawImageState(struct frame *f)
{
	gfx_drawimage(PTR(A(0)), PTR(A(1)), (int16_t)D(0), (int16_t)D(1));
}

static void i_PrintIText(struct frame *f)
{
	gfx_printitext(PTR(A(0)), PTR(A(1)), (int16_t)D(0), (int16_t)D(1));
}

static void i_IntuiTextLength(struct frame *f)
{
	RET(gfx_itextlength(PTR(A(0))));
}

static void i_EraseImage(struct frame *f)
{
	struct Image *im = PTR(A(1));
	int x = (int16_t)D(0) + im->LeftEdge, y = (int16_t)D(1) + im->TopEdge;

	gfx_rectfill(PTR(A(0)), x, y, x + im->Width - 1, y + im->Height - 1, 0);
}

/* ---- menus ---- */

static const char *item_text(uint32_t fill, uint16_t flags)
{
	struct IntuiText *it;

	if (!(flags & ITEMTEXT) || !fill)
		return NULL;
	it = PTR(fill);
	return it->IText ? PTR(it->IText) : "";
}

static void send_menus(struct awin *a)
{
	struct wbp_menuitem *m = calloc(512, sizeof(*m));
	struct Menu *mn;
	int n = 0, mi;

	for (mn = PTR(a->w->MenuStrip), mi = 0; mn && n < 500;
	     mn = PTR(mn->NextMenu), mi++) {
		struct MenuItem *it;
		int ii;
		m[n].kind = WBP_MENU;
		m[n].flags = (mn->Flags & MENUENABLED) ? 0 : WBP_MI_DISABLED;
		snprintf(m[n].label, sizeof(m[n].label), "%s",
			 mn->MenuName ? (char *)PTR(mn->MenuName) : "");
		n++;
		for (it = PTR(mn->FirstItem), ii = 0; it && n < 500;
		     it = PTR(it->NextItem), ii++) {
			const char *t = item_text(it->ItemFill, it->Flags);
			struct MenuItem *sub;
			int si;
			if (!t) {
				m[n++].kind = WBP_BAR;
				continue;
			}
			m[n].kind = WBP_ITEM;
			m[n].id = (31 << 11) | (ii << 5) | mi;
			m[n].flags = (it->Flags & ITEMENABLED) ? 0 : WBP_MI_DISABLED;
			if (it->Flags & CHECKIT)
				m[n].flags |= WBP_MI_CHECKIT;
			if (it->Flags & CHECKED)
				m[n].flags |= WBP_MI_CHECKED;
			if (it->Flags & COMMSEQ)
				m[n].key = it->Command;
			snprintf(m[n].label, sizeof(m[n].label), "%s", t);
			n++;
			for (sub = PTR(it->SubItem), si = 0; sub && n < 500;
			     sub = PTR(sub->NextItem), si++) {
				const char *st = item_text(sub->ItemFill, sub->Flags);
				if (!st)
					continue;       /* no bars in submenus */
				m[n].kind = WBP_SUBITEM;
				m[n].id = (si << 11) | (ii << 5) | mi;
				m[n].flags = (sub->Flags & ITEMENABLED) ? 0 :
					     WBP_MI_DISABLED;
				if (sub->Flags & CHECKIT)
					m[n].flags |= WBP_MI_CHECKIT;
				if (sub->Flags & CHECKED)
					m[n].flags |= WBP_MI_CHECKED;
				if (sub->Flags & COMMSEQ)
					m[n].key = sub->Command;
				snprintf(m[n].label, sizeof(m[n].label), "%s", st);
				n++;
			}
		}
	}
	gui_send(WBP_SET_MENUS, a->id, m, n * sizeof(*m));
	free(m);
}

static struct MenuItem *item_address(uint32_t strip, uint16_t num,
				     struct MenuItem **parentlist)
{
	struct Menu *mn = PTR(strip);
	struct MenuItem *it;
	int m = num & 31, i = (num >> 5) & 63, s = (num >> 11) & 31;

	if (num == MENUNULL)
		return NULL;
	while (mn && m--)
		mn = PTR(mn->NextMenu);
	if (!mn || i == 63)
		return NULL;
	it = PTR(mn->FirstItem);
	while (it && i--)
		it = PTR(it->NextItem);
	if (parentlist)
		*parentlist = PTR(mn->FirstItem);
	if (!it || s == 31)
		return it;
	if (parentlist)
		*parentlist = PTR(it->SubItem);
	it = PTR(it->SubItem);
	while (it && s--)
		it = PTR(it->NextItem);
	return it;
}

void menu_picked(struct awin *a, uint16_t code)
{
	struct MenuItem *list, *it = item_address(a->w->MenuStrip, code, &list);
	int k = 0;

	if (!it)
		return;
	it->NextSelect = MENUNULL;
	if (!(it->Flags & CHECKIT))
		return;
	if (it->Flags & MENUTOGGLE)
		it->Flags ^= CHECKED;
	else
		it->Flags |= CHECKED;
	for (; list; list = PTR(list->NextItem), k++)
		if (list != it && (it->MutualExclude & (1 << k)))
			list->Flags &= ~CHECKED;
	send_menus(a);
}

static void i_SetMenuStrip(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct Window *w = PTR(A(0));

	w->MenuStrip = A(1);
	if (a)
		send_menus(a);
	RET(1);
}

static void i_ClearMenuStrip(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct Window *w = PTR(A(0));

	w->MenuStrip = 0;
	if (a)
		gui_send(WBP_SET_MENUS, a->id, NULL, 0);
}

static void i_ResetMenuStrip(struct frame *f)
{
	i_SetMenuStrip(f);
}

static void i_ItemAddress(struct frame *f)
{
	RET(item_address(A(0), D(0), NULL));
}

static void on_off_menu(struct frame *f, int on)
{
	struct Window *w = PTR(A(0));
	struct awin *a = gui_by_window(A(0));
	uint16_t num = D(0);
	struct MenuItem *it;

	if ((num >> 5 & 63) == 63) {            /* the whole menu */
		struct Menu *mn = PTR(w->MenuStrip);
		int m = num & 31;
		while (mn && m--)
			mn = PTR(mn->NextMenu);
		if (mn)
			mn->Flags = on ? mn->Flags | MENUENABLED :
				    mn->Flags & ~MENUENABLED;
	} else if ((it = item_address(w->MenuStrip, num, NULL)))
		it->Flags = on ? it->Flags | ITEMENABLED : it->Flags & ~ITEMENABLED;
	if (a)
		send_menus(a);
}

static void i_OnMenu(struct frame *f) { on_off_menu(f, 1); }
static void i_OffMenu(struct frame *f) { on_off_menu(f, 0); }

/* ---- requesters ---- */

struct sbuf {
	char s[2048];
	int n;
};

static void put_sbuf(int c, void *ctx)
{
	struct sbuf *b = ctx;
	if (b->n < (int)sizeof(b->s) - 1)
		b->s[b->n++] = c;
	b->s[b->n] = 0;
}

/* ask on the terminal: gadgets "A|B|C" give 1, 2, ... and 0 for the last */
static int ask_tty(const char *title, const char *body, const char *gadgets)
{
	char line[64], g[1024], *opt[16];
	int n = 0, i, tty = isatty(0) && isatty(2);

	dos_flush_all();
	snprintf(g, sizeof(g), "%s", gadgets);
	for (char *s = strtok(g, "|"); s && n < 16; s = strtok(NULL, "|"))
		opt[n++] = s;
	fprintf(stderr, "\n[%s]\n%s\n", title ? title : "Request", body);
	if (!n)
		return 0;
	for (i = 0; i < n; i++)
		fprintf(stderr, "  %d: %s\n", i + 1, opt[i]);
	if (!tty) {
		fprintf(stderr, "(no terminal: choosing \"%s\")\n", opt[n - 1]);
		return 0;
	}
	for (;;) {
		fprintf(stderr, "choice [1-%d]: ", n);
		if (!fgets(line, sizeof(line), stdin))
			return 0;
		i = atoi(line);
		if (i >= 1 && i <= n)
			return i == n ? 0 : i;
	}
}

/* an EasyRequest as a window of ours: body lines and a row of buttons */
struct easy {
	int nb;
	int bx[16], bw[16], by, bh;
	int pressed;
};
static struct easy easies[8];

static struct easy *easy_of(struct awin *a)
{
	return &easies[(a->easy - 1) & 7];
}

void intui_easy_event(struct awin *a, const struct wbp_event *e)
{
	struct easy *ez = easy_of(a);
	int x = e->mousex, y = e->mousey, i;

	if (e->class == IDCMP_MOUSEBUTTONS && e->code == SELECTDOWN) {
		ez->pressed = -1;
		for (i = 0; i < ez->nb; i++)
			if (x >= ez->bx[i] && x < ez->bx[i] + ez->bw[i] &&
			    y >= ez->by && y < ez->by + ez->bh) {
				ez->pressed = i;
				gfx_complement(a->rp, ez->bx[i] + a->bl + 1,
					       ez->by + a->bt + 1,
					       ez->bx[i] + a->bl + ez->bw[i] - 2,
					       ez->by + a->bt + ez->bh - 2);
			}
	} else if (e->class == IDCMP_MOUSEBUTTONS && e->code == SELECTUP &&
		   ez->pressed >= 0) {
		i = ez->pressed;
		ez->pressed = -1;
		gfx_complement(a->rp, ez->bx[i] + a->bl + 1, ez->by + a->bt + 1,
			       ez->bx[i] + a->bl + ez->bw[i] - 2,
			       ez->by + a->bt + ez->bh - 2);
		if (x >= ez->bx[i] && x < ez->bx[i] + ez->bw[i] &&
		    y >= ez->by && y < ez->by + ez->bh)
			a->easy_result = i == ez->nb - 1 ? 0 : i + 1;
	} else if (e->class == IDCMP_VANILLAKEY &&
		   (e->code == '\r' || e->code == 27)) {
		a->easy_result = e->code == 27 ? 0 : (ez->nb > 1 ? 1 : 0);
	} else if (e->class == IDCMP_CLOSEWINDOW)
		a->easy_result = 0;
}

static struct awin *easy_open(const char *title, const char *body,
			      const char *gadgets)
{
	char *lines[32], *gl[16], bodyc[2048], gadc[1024];
	int nl = 0, ng = 0, i, w = 0, gw = 0, fh, cw, iw, ih;
	struct wspec s;
	struct Window *win;
	struct awin *a;
	struct easy *ez;
	static int next;

	if (gui_connect())
		return NULL;
	fh = gui_topaz->tf_YSize;
	cw = gui_topaz->tf_XSize;
	snprintf(bodyc, sizeof(bodyc), "%s", body);
	snprintf(gadc, sizeof(gadc), "%s", gadgets);
	for (char *p = bodyc, *q; p && nl < 32; p = q) {
		q = strchr(p, '\n');
		if (q)
			*q++ = 0;
		lines[nl++] = p;
		if ((int)strlen(p) * cw > w)
			w = strlen(p) * cw;
	}
	for (char *p = strtok(gadc, "|"); p && ng < 16; p = strtok(NULL, "|"))
		gl[ng++] = p;
	for (i = 0; i < ng; i++)
		gw += strlen(gl[i]) * cw + 16 + 8;
	iw = (w > gw ? w : gw) + 32;
	ih = nl * (fh + 1) + fh + 12 + 20;
	memset(&s, 0, sizeof(s));
	s.iw = iw;
	s.ih = ih;
	s.left = (gui_screen.width - iw) / 2;
	s.top = (gui_screen.height - ih) / 2;
	s.flags = WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_RMBTRAP;
	s.title = APTR_OF(strcpy(amem_alloc(strlen(title) + 1), title));
	win = open_window(&s);
	if (!win)
		return NULL;
	a = gui_by_window(APTR_OF(win));
	a->easy = (next++ & 7) + 1;
	a->easy_result = -1;
	ez = easy_of(a);
	memset(ez, 0, sizeof(*ez));
	ez->nb = ng;
	gfx_rectfill(a->rp, a->bl, a->bt, a->bl + a->iw - 1, a->bt + a->ih - 1,
		     gui_pens[WBP_BACKGROUNDPEN]);
	gfx_bevel(a->rp, a->bl + 4, a->bt + 3, a->iw - 8, nl * (fh + 1) + 8, 1);
	a->rp->DrawMode = JAM1;
	a->rp->FgPen = gui_pens[WBP_TEXTPEN];
	for (i = 0; i < nl; i++) {
		a->rp->cp_x = a->bl + 16;
		a->rp->cp_y = a->bt + 8 + i * (fh + 1) + gui_topaz->tf_Baseline;
		gfx_text(a->rp, lines[i], strlen(lines[i]));
	}
	ez->by = nl * (fh + 1) + 16;
	ez->bh = fh + 6;
	for (i = 0; i < ng; i++) {
		int bw = strlen(gl[i]) * cw + 16;
		int bx = ng == 1 ? (a->iw - bw) / 2 :
			 i == 0 ? 8 : i == ng - 1 ? a->iw - bw - 8 :
			 8 + (a->iw - 16 - bw) * i / (ng - 1);
		ez->bx[i] = bx;
		ez->bw[i] = bw;
		gfx_bevel(a->rp, a->bl + bx, a->bt + ez->by, bw, ez->bh, 0);
		a->rp->cp_x = a->bl + bx + 8;
		a->rp->cp_y = a->bt + ez->by + 3 + gui_topaz->tf_Baseline;
		gfx_text(a->rp, gl[i], strlen(gl[i]));
	}
	gui_flush();
	return a;
}

static int easy_wait(struct awin *a)
{
	int r;

	while (a->easy_result < 0) {
		gui_flush();
		if (gui_fd() < 0)
			break;
		gui_pump(-1);
	}
	r = a->easy_result < 0 ? 0 : a->easy_result;
	gui_close(a);
	gui_flush();
	return r;
}

static void format_easy(uint32_t *es, uint32_t data, struct sbuf *body,
			struct sbuf *gad)
{
	if (es[3])
		data = fmt_do(PTR(es[3]), data, put_sbuf, body, 0);
	if (es[4])
		fmt_do(PTR(es[4]), data, put_sbuf, gad, 0);
}

static void i_EasyRequestArgs(struct frame *f)
{
	uint32_t *es = PTR(A(1));
	struct sbuf body = { "", 0 }, gad = { "", 0 };
	const char *title;
	struct awin *a;

	if (!es) {
		RET(0);
		return;
	}
	format_easy(es, A(3), &body, &gad);
	title = es[2] ? PTR(es[2]) : "Request";
	a = easy_open(title, body.s, gad.s);
	RET(a ? easy_wait(a) : ask_tty(title, body.s, gad.s));
}

static void i_BuildEasyRequestArgs(struct frame *f)
{
	uint32_t *es = PTR(A(1));
	struct sbuf body = { "", 0 }, gad = { "", 0 };
	struct awin *a;

	if (!es) {
		RET(0);
		return;
	}
	format_easy(es, A(3), &body, &gad);
	a = easy_open(es[2] ? PTR(es[2]) : "Request", body.s, gad.s);
	RET(a ? APTR_OF(a->w) : 1);     /* 1: "it was answered at once" */
}

static void i_SysReqHandler(struct frame *f)
{
	struct awin *a = A(0) > 1 ? gui_by_window(A(0)) : NULL;

	if (!a) {
		RET(0);
		return;
	}
	gui_flush();
	gui_pump(D(0) ? -1 : 0);
	RET(a->easy_result < 0 ? -2 : a->easy_result);
}

static void i_FreeSysRequest(struct frame *f)
{
	struct awin *a = A(0) > 1 ? gui_by_window(A(0)) : NULL;

	if (a) {
		gui_close(a);
		gui_flush();
	}
}

/* IntuiText chains: IText at +12, NextText at +16 */
static void itext(uint32_t it, char *buf, size_t len)
{
	size_t n = 0;

	buf[0] = 0;
	for (; it && n < len - 2; it = ((struct IntuiText *)PTR(it))->NextText) {
		uint32_t t = ((struct IntuiText *)PTR(it))->IText;
		n += snprintf(buf + n, len - n, "%s%s", n ? "\n" : "",
			      t ? (char *)PTR(t) : "");
	}
}

static void i_AutoRequest(struct frame *f)
{
	char body[1024], pos[128], neg[128], g[260];
	struct awin *a;

	itext(A(1), body, sizeof(body));
	itext(A(2), pos, sizeof(pos));
	itext(A(3), neg, sizeof(neg));
	if (*pos)
		snprintf(g, sizeof(g), "%s|%s", pos, neg);
	else
		snprintf(g, sizeof(g), "%s", neg);
	a = easy_open("System Request", body, g);
	if (a)
		RET(*pos ? easy_wait(a) == 1 : (easy_wait(a), 0));
	else
		RET(*pos ? ask_tty(NULL, body, g) == 1 : 0);
}

static void i_DisplayBeep(struct frame *f)
{
	if (isatty(2) && write(2, "\a", 1) < 0) {
		/* no beep then */
	}
}

static void i_DisplayAlert(struct frame *f)
{
	const uint8_t *s = PTR(A(0));

	dos_flush_all();
	fprintf(stderr, "amigafmt: %s: alert $%08x:", af_progname, D(0));
	/* lines: x(WORD) y(BYTE) text NUL continuation */
	while (s && (*s || s[1])) {
		s += 3;
		fprintf(stderr, " %s", s);
		s += strlen((const char *)s) + 1;
		if (!*s++)
			break;
	}
	fputc('\n', stderr);
	RET(0);
}

/* ---- the screen ---- */

static void i_LockPubScreen(struct frame *f)
{
	const char *n = A(0) ? PTR(A(0)) : NULL;

	if ((n && strcmp(n, "Workbench")) || gui_connect()) {
		RET(0);
		return;
	}
	RET(wb_screen);
}

static void i_OpenWorkBench(struct frame *f)
{
	RET(gui_connect() ? 0 : APTR_OF(wb_screen));
}

static void i_GetScreenDrawInfo(struct frame *f)
{
	RET(gui_connect() ? 0 : APTR_OF(gui_dri));
}

static void i_GetScreenData(struct frame *f)
{
	if (gui_connect()) {
		RET(0);
		return;
	}
	memcpy(PTR(A(0)), wb_screen, D(0) < sizeof(struct Screen) ? D(0) :
	       sizeof(struct Screen));
	RET(1);
}

static void i_GetDefaultPubScreen(struct frame *f)
{
	if (A(0))
		strcpy(PTR(A(0)), "Workbench");
}

static void i_ViewPortAddress(struct frame *f)
{
	RET(gui_connect() ? 0 : APTR_OF(&wb_screen->ViewPort));
}

static void i_NextPubScreen(struct frame *f)
{
	RET(0);
}

static void i_PubScreenStatus(struct frame *f) { RET(0); }

static void i_LockPubScreenList(struct frame *f)
{
	static struct List l;
	extern void exec_newlist(void *l);

	exec_newlist(&l);
	RET(&l);
}

static void i_CurrentTime(struct frame *f)
{
	struct timeval tv;
	struct DateStamp ds;

	gettimeofday(&tv, NULL);
	date_from_unix(tv.tv_sec, &ds);
	*(uint32_t *)PTR(A(0)) = ds.ds_Days * 86400 + ds.ds_Minute * 60 +
				 ds.ds_Tick / 50;
	*(uint32_t *)PTR(A(1)) = tv.tv_usec;
}

static void i_DoubleClick(struct frame *f)
{
	int64_t a = (int64_t)D(0) * 1000000 + D(1);
	int64_t b = (int64_t)D(2) * 1000000 + D(3);

	RET(b - a >= 0 && b - a < 500000);
}

/* struct Preferences: FontHeight first, the rest zero is acceptable */
static void i_GetPrefs(struct frame *f)
{
	uint8_t *p = PTR(A(0));

	memset(p, 0, D(0));
	if (D(0) > 0)
		p[0] = 8;                       /* TOPAZ_EIGHTY */
	RET(A(0));
}

static void i_SetWindowPointerA(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	struct TagItem *t = PTR(A(1));
	uint32_t busy = 0;

	for (; t && t->ti_Tag; t++)
		if (t->ti_Tag == 0x80000098)            /* WA_BusyPointer */
			busy = t->ti_Data != 0;
	if (a)
		gui_send(WBP_SET_POINTER, a->id, &busy, sizeof(busy));
}

static void i_ClearPointer(struct frame *f)
{
	struct awin *a = gui_by_window(A(0));
	uint32_t busy = 0;

	if (a)
		gui_send(WBP_SET_POINTER, a->id, &busy, sizeof(busy));
}

/* Remember lists: { NextRemember, RememberSize, Memory } */
static void i_AllocRemember(struct frame *f)
{
	uint32_t *key = PTR(A(0)), *r = amem_alloc(12);
	void *m = amem_alloc(D(0) ? D(0) : 1);

	r[0] = *key;
	r[1] = D(0);
	r[2] = APTR_OF(m);
	*key = APTR_OF(r);
	RET(m);
}

static void i_FreeRemember(struct frame *f)
{
	uint32_t *key = PTR(A(0)), *r, *n;

	for (r = PTR(*key); r; r = n) {
		n = PTR(r[0]);
		if (D(0))
			amem_free(PTR(r[2]));
		amem_free(r);
	}
	*key = 0;
}

static void i_Zero(struct frame *f) { RET(0); }

/* QueryOverscan(displayID, rect, type): every overscan is the screen */
static void i_QueryOverscan(struct frame *f)
{
	int16_t *r = PTR(A(1));

	gui_connect();
	r[0] = r[1] = 0;
	r[2] = (gui_screen.width ? gui_screen.width : 640) - 1;
	r[3] = (gui_screen.height ? gui_screen.height : 200) - 1;
	RET(1);
}
static void i_True(struct frame *f) { RET(1); }
static void i_Nop(struct frame *f) { (void)f; }

static const struct libimpl intuition_impl[] = {
	{ "OpenWindow", i_OpenWindow },
	{ "OpenWindowTagList", i_OpenWindowTagList },
	{ "CloseWindow", i_CloseWindow }, { "ModifyIDCMP", i_ModifyIDCMP },
	{ "SetWindowTitles", i_SetWindowTitles },
	{ "ActivateWindow", i_ActivateWindow },
	{ "WindowToFront", i_WindowToFront }, { "WindowToBack", i_WindowToBack },
	{ "MoveWindow", i_MoveWindow }, { "SizeWindow", i_SizeWindow },
	{ "ChangeWindowBox", i_ChangeWindowBox },
	{ "WindowLimits", i_WindowLimits },
	{ "RefreshWindowFrame", i_RefreshWindowFrame },
	{ "BeginRefresh", i_Nop }, { "EndRefresh", i_Nop },
	{ "ReportMouse", i_Nop },
	{ "AddGadget", i_AddGadget }, { "AddGList", i_AddGList },
	{ "RemoveGadget", i_RemoveGadget }, { "RemoveGList", i_RemoveGList },
	{ "RefreshGadgets", i_RefreshGadgets }, { "RefreshGList", i_RefreshGList },
	{ "OnGadget", i_OnGadget }, { "OffGadget", i_OffGadget },
	{ "ActivateGadget", i_ActivateGadget },
	{ "ModifyProp", i_ModifyProp }, { "NewModifyProp", i_NewModifyProp },
	{ "DrawBorder", i_DrawBorder }, { "DrawImage", i_DrawImage },
	{ "DrawImageState", i_DrawImageState }, { "EraseImage", i_EraseImage },
	{ "PrintIText", i_PrintIText }, { "IntuiTextLength", i_IntuiTextLength },
	{ "SetMenuStrip", i_SetMenuStrip }, { "ClearMenuStrip", i_ClearMenuStrip },
	{ "ResetMenuStrip", i_ResetMenuStrip }, { "ItemAddress", i_ItemAddress },
	{ "OnMenu", i_OnMenu }, { "OffMenu", i_OffMenu },
	{ "EasyRequestArgs", i_EasyRequestArgs },
	{ "BuildEasyRequestArgs", i_BuildEasyRequestArgs },
	{ "SysReqHandler", i_SysReqHandler },
	{ "FreeSysRequest", i_FreeSysRequest },
	{ "AutoRequest", i_AutoRequest },
	{ "DisplayBeep", i_DisplayBeep }, { "DisplayAlert", i_DisplayAlert },
	{ "LockPubScreen", i_LockPubScreen }, { "UnlockPubScreen", i_Nop },
	{ "OpenWorkBench", i_OpenWorkBench }, { "WBenchToFront", i_True },
	{ "WBenchToBack", i_True }, { "CloseWorkBench", i_Zero },
	{ "GetScreenDrawInfo", i_GetScreenDrawInfo },
	{ "FreeScreenDrawInfo", i_Nop }, { "GetScreenData", i_GetScreenData },
	{ "GetDefaultPubScreen", i_GetDefaultPubScreen },
	{ "SetDefaultPubScreen", i_Nop }, { "SetPubScreenModes", i_Zero },
	{ "PubScreenStatus", i_PubScreenStatus },
	{ "LockPubScreenList", i_LockPubScreenList },
	{ "UnlockPubScreenList", i_Nop }, { "NextPubScreen", i_NextPubScreen },
	{ "ViewPortAddress", i_ViewPortAddress },
	{ "OpenScreen", i_Zero }, { "OpenScreenTagList", i_Zero },
	{ "CloseScreen", i_True }, { "ScreenToFront", i_Nop },
	{ "ScreenToBack", i_Nop }, { "ShowTitle", i_Nop },
	{ "MakeScreen", i_Zero }, { "RethinkDisplay", i_Zero },
	{ "RemakeDisplay", i_Zero }, { "ViewAddress", i_Zero },
	{ "CurrentTime", i_CurrentTime }, { "DoubleClick", i_DoubleClick },
	{ "GetPrefs", i_GetPrefs }, { "GetDefPrefs", i_GetPrefs },
	{ "SetPrefs", i_Zero },
	{ "SetPointer", i_Nop }, { "ClearPointer", i_ClearPointer },
	{ "SetWindowPointerA", i_SetWindowPointerA },
	{ "AllocRemember", i_AllocRemember }, { "FreeRemember", i_FreeRemember },
	{ "LockIBase", i_Zero }, { "UnlockIBase", i_Nop },
	{ "SetMouseQueue", i_Zero }, { "AlohaWorkbench", i_Nop },
	{ "QueryOverscan", i_QueryOverscan },
	{ NULL, NULL }
};

static void intuition_setup(struct Library *b)
{
	extern void exec_newlist(void *l);

	IntuitionBase = b;
	imsg_port = amem_alloc(sizeof(*imsg_port));
	imsg_port->mp_Node.ln_Type = NT_MSGPORT;
	imsg_port->mp_Flags = 2;                /* PA_IGNORE */
	exec_newlist(&imsg_port->mp_MsgList);
}

struct amilib lib_intuition = {
	.name = "intuition.library", .version = 40, .revision = 85,
	.fd = fd_intuition, .impl = intuition_impl,
	.possize = 0x200,               /* IntuitionBase */
	.init = intuition_setup,
};
