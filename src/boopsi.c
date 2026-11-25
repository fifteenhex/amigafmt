/*
 * BOOPSI: Intuition's object system.  Classes are real IClass structures
 * and objects carry the real 12-byte header in front of them, so a
 * program's own classes (MakeClass + its dispatcher hook) and its calls
 * to DoMethod/DoSuperMethod work unchanged.  The system classes are
 * implemented here in C; their dispatcher hooks point at stubs of a
 * hidden library, so a program's DoSuperMethod lands in C the same way a
 * library call does.
 *
 * System classes: rootclass, imageclass, frameiclass, sysiclass,
 * fillrectclass, itexticlass, gadgetclass, buttongclass, frbuttonclass,
 * propgclass, strgclass, icclass, modelclass.  Custom gadgets are driven
 * from intuition.c through boopsi_gadget_*().
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gui.h"

#define OM_NEW       0x101
#define OM_DISPOSE   0x102
#define OM_SET       0x103
#define OM_GET       0x104
#define OM_ADDTAIL   0x105
#define OM_REMOVE    0x106
#define OM_NOTIFY    0x107
#define OM_UPDATE    0x108
#define OM_ADDMEMBER 0x109
#define OM_REMMEMBER 0x10a
#define GM_HITTEST   0
#define GM_RENDER    1
#define GM_GOACTIVE  2
#define GM_HANDLEINPUT 3
#define GM_GOINACTIVE 4
#define GM_LAYOUT    6
#define IM_DRAW      0x202
#define IM_HITTEST   0x203
#define IM_ERASE     0x204
#define IM_DRAWFRAME 0x206
#define IM_FRAMEBOX  0x207
#define IM_HITFRAME  0x208
#define IM_ERASEFRAME 0x209

#define GMR_GADGETHIT 4
#define GMR_MEACTIVE 0
#define GMR_NOREUSE  2
#define GMR_REUSE    4
#define GMR_VERIFY   8

#define IA(n)  (0x80020000 + (n))
#define GA(n)  (0x80030000 + (n))
#define ICA_TARGET 0x80040001
#define ICA_MAP    0x80040002
#define PGA(n) (0x80031000 + (n))
#define STRINGA(n) (0x80032000 + (n))
#define ICTARGET_IDCMP 0xffffffff

#define GTYP_CUSTOMGADGET 5
#define IDCMP_IDCMPUPDATE 0x00800000
#define IDCMP_GADGETUP    0x00000040
#define IDCMP_GADGETDOWN  0x00000020

/* struct IClass */
struct IClass {
	struct Hook cl_Dispatcher;
	uint32_t cl_Reserved;
	APTR cl_Super, cl_ID;
	uint16_t cl_InstOffset, cl_InstSize;
	uint32_t cl_UserData, cl_SubclassCount, cl_ObjectCount, cl_Flags;
};

#define OCLASS(o) ((struct IClass *)PTR(((uint32_t *)(o))[-1]))
#define INST(cl, o) ((uint8_t *)(o) + (cl)->cl_InstOffset)

extern struct frame *resume_frame;
extern struct TagItem *util_next_tag(struct TagItem **tp);
extern void intui_put_imsg(struct awin *a, uint32_t class, uint16_t code,
			   uint16_t qual, APTR iaddr, int mx, int my);

struct pubclass {
	char name[32];
	struct IClass *cl;
	struct pubclass *next;
};
static struct pubclass *pubs;

static uint32_t tagdata(struct TagItem *t, uint32_t tag, uint32_t def)
{
	struct TagItem *x;

	while ((x = util_next_tag(&t)))
		if (x->ti_Tag == tag)
			return x->ti_Data;
	return def;
}

static int hastag(struct TagItem *t, uint32_t tag)
{
	struct TagItem *x;

	while ((x = util_next_tag(&t)))
		if (x->ti_Tag == tag)
			return 1;
	return 0;
}

/* call a class's dispatcher: A0 class, A2 object, A1 message */
uint32_t boopsi_do(struct IClass *cl, uint32_t obj, void *msg)
{
	struct regs r;
	struct frame dummy;
	struct frame *f = resume_frame;

	if (!cl || !cl->cl_Dispatcher.h_Entry)
		return 0;
	memset(&r, 0, sizeof(r));
	r.a[0] = APTR_OF(cl);
	r.a[1] = APTR_OF(msg);
	r.a[2] = obj;
	r.a[6] = APTR_OF(SysBase);
	if (!f) {
		f = &dummy;
		return 0;
	}
	return amiga_call(f, cl->cl_Dispatcher.h_Entry, &r);
}

static uint32_t do_method(uint32_t obj, void *msg)
{
	return obj ? boopsi_do(OCLASS(PTR(obj)), obj, msg) : 0;
}

static uint32_t super_method(struct IClass *cl, uint32_t obj, void *msg)
{
	return boopsi_do(PTR(cl->cl_Super), obj, msg);
}

/* messages live in Amiga memory: small ones are built in this pool */
static uint32_t *msgbuf(int longs)
{
	static uint32_t *pool;
	static int at;

	if (!pool)
		pool = amem_alloc(4096);
	if (at + longs > 1024)
		at = 0;
	at += longs;
	return pool + at - longs;
}

/* ---- GadgetInfo for a window ---- */

static uint8_t *ginfo(struct awin *a)
{
	uint8_t *gi;

	if (!a)
		return NULL;
	gi = amem_alloc(64);
	*(uint32_t *)(gi + 0) = APTR_OF(wb_screen);
	*(uint32_t *)(gi + 4) = APTR_OF(a->w);
	*(uint32_t *)(gi + 12) = APTR_OF(a->rp);
	*(uint32_t *)(gi + 16) = a->w->WLayer;
	*(int16_t *)(gi + 24) = a->w->Width;
	*(int16_t *)(gi + 26) = a->w->Height;
	gi[28] = gui_pens[WBP_DETAILPEN];
	gi[29] = gui_pens[WBP_BLOCKPEN];
	*(uint32_t *)(gi + 30) = APTR_OF(gui_dri);
	return gi;
}

static struct awin *ginfo_win(uint32_t gi)
{
	return gi ? gui_by_window(*(uint32_t *)((uint8_t *)PTR(gi) + 4)) : NULL;
}

/* ---- notification (gadgetclass and icclass): ICA_TARGET / ICA_MAP ---- */

struct notify {
	uint32_t target, map;
};

static void notify(struct notify *n, uint32_t obj, uint32_t tags, uint32_t gi,
		   uint32_t flags)
{
	struct TagItem *src = PTR(tags), *x, *out;
	int cnt = 0, i;

	if (!n->target || !tags)
		return;
	for (x = src; util_next_tag(&x); )
		cnt++;
	out = amem_alloc((cnt + 2) * sizeof(*out));
	for (x = src, i = 0; (x = util_next_tag(&src)); i++) {
		uint32_t tag = x->ti_Tag;
		if (n->map) {
			struct TagItem *m = PTR(n->map), *y;
			while ((y = util_next_tag(&m)))
				if (y->ti_Tag == tag) {
					tag = y->ti_Data;
					break;
				}
		}
		out[i].ti_Tag = tag;
		out[i].ti_Data = x->ti_Data;
	}
	if (n->target == ICTARGET_IDCMP) {
		struct awin *a = ginfo_win(gi);
		uint16_t code = 0;
		struct Gadget *g = PTR(obj);
		(void)g;
		if (a)
			intui_put_imsg(a, IDCMP_IDCMPUPDATE, code, 0, APTR_OF(out),
				       a->w->MouseX, a->w->MouseY);
		return;
	}
	{
		uint32_t *m = msgbuf(4);
		m[0] = OM_UPDATE;
		m[1] = APTR_OF(out);
		m[2] = gi;
		m[3] = flags;
		do_method(n->target, m);
	}
}

/* ---- rootclass ---- */

static void cls_root(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);

	(void)cl;
	switch (msg[0]) {
	case OM_NEW: {
		struct IClass *true_cl = PTR(obj);      /* the class being made */
		uint32_t size = true_cl->cl_InstOffset + true_cl->cl_InstSize;
		uint32_t *mem = amem_alloc(12 + size);
		mem[2] = APTR_OF(true_cl);
		true_cl->cl_ObjectCount++;
		RET(mem + 3);
		return;
	}
	case OM_DISPOSE:
		OCLASS(PTR(obj))->cl_ObjectCount--;
		amem_free((uint32_t *)PTR(obj) - 3);
		RET(0);
		return;
	case OM_ADDTAIL: {
		extern void exec_addtail(void *l, void *n);
		exec_addtail(PTR(msg[1]), (uint32_t *)PTR(obj) - 3);
		RET(0);
		return;
	}
	case OM_REMOVE: {
		struct MinNode *n = (struct MinNode *)((uint32_t *)PTR(obj) - 3);
		((struct MinNode *)PTR(n->mln_Pred))->mln_Succ = n->mln_Succ;
		((struct MinNode *)PTR(n->mln_Succ))->mln_Pred = n->mln_Pred;
		RET(0);
		return;
	}
	}
	RET(0);
}

/* ---- imageclass: the instance is a struct Image ---- */

static int image_set(struct Image *im, struct TagItem *t)
{
	struct TagItem *x;
	int n = 0;

	while ((x = util_next_tag(&t))) {
		switch (x->ti_Tag) {
		case IA(1): im->LeftEdge = x->ti_Data; break;
		case IA(2): im->TopEdge = x->ti_Data; break;
		case IA(3): im->Width = x->ti_Data; break;
		case IA(4): im->Height = x->ti_Data; break;
		case IA(5): im->PlanePick = x->ti_Data; break;          /* FGPen */
		case IA(6): im->PlaneOnOff = x->ti_Data; break;         /* BGPen */
		case IA(7): im->ImageData = x->ti_Data; break;
		default: continue;
		}
		n++;
	}
	return n;
}

static void cls_image(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct Image *im;

	switch (msg[0]) {
	case OM_NEW:
		obj = super_method(cl, obj, msg);
		if (obj) {
			im = PTR(obj);
			im->Depth = -1;         /* CUSTOMIMAGEDEPTH */
			im->PlanePick = 1;
			image_set(im, PTR(msg[1]));
		}
		RET(obj);
		return;
	case OM_SET:
		RET(image_set(PTR(obj), PTR(msg[1])));
		return;
	case OM_GET: {
		uint32_t *st = PTR(msg[2]);
		im = PTR(obj);
		switch (msg[1]) {
		case IA(1): *st = im->LeftEdge; break;
		case IA(2): *st = im->TopEdge; break;
		case IA(3): *st = im->Width; break;
		case IA(4): *st = im->Height; break;
		case IA(7): *st = im->ImageData; break;
		default: RET(super_method(cl, obj, msg)); return;
		}
		RET(1);
		return;
	}
	case IM_HITTEST:
	case IM_HITFRAME: {
		int16_t px = msg[1] >> 16, py = msg[1];
		int w, h;
		im = PTR(obj);
		w = msg[0] == IM_HITFRAME ? (int16_t)(msg[2] >> 16) : im->Width;
		h = msg[0] == IM_HITFRAME ? (int16_t)msg[2] : im->Height;
		RET(px >= im->LeftEdge && py >= im->TopEdge &&
		    px < im->LeftEdge + w && py < im->TopEdge + h);
		return;
	}
	case IM_ERASE:
	case IM_ERASEFRAME: {
		int16_t x = msg[2] >> 16, y = msg[2];
		int w, h;
		im = PTR(obj);
		w = msg[0] == IM_ERASEFRAME ? (int16_t)(msg[3] >> 16) : im->Width;
		h = msg[0] == IM_ERASEFRAME ? (int16_t)msg[3] : im->Height;
		gfx_rectfill(PTR(msg[1]), x + im->LeftEdge, y + im->TopEdge,
			     x + im->LeftEdge + w - 1, y + im->TopEdge + h - 1, 0);
		RET(0);
		return;
	}
	case IM_DRAWFRAME: {
		/* imageclass: draw at the given size by drawing normally */
		uint32_t *m = msgbuf(5);
		memcpy(m, msg, 20);
		m[0] = IM_DRAW;
		RET(do_method(obj, m));
		return;
	}
	case IM_FRAMEBOX: {
		int16_t *cb = PTR(msg[1]), *fb = PTR(msg[2]);
		memcpy(fb, cb, 8);
		RET(0);
		return;
	}
	case IM_DRAW:
		RET(0);
		return;
	}
	RET(super_method(cl, obj, msg));
}

/* frameiclass: a bevel, recessed or raised, around (or as) the image */
struct frinst {
	int recessed, edgesonly, ftype;
};

static void cls_frame(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct frinst *fi;

	switch (msg[0]) {
	case OM_NEW:
		obj = super_method(cl, obj, msg);
		if (obj) {
			fi = (struct frinst *)INST(cl, PTR(obj));
			fi->recessed = tagdata(PTR(msg[1]), IA(0x15), 0) != 0;
			fi->edgesonly = tagdata(PTR(msg[1]), IA(0x17), 0) != 0;
			fi->ftype = tagdata(PTR(msg[1]), IA(0x1b), 0);
		}
		RET(obj);
		return;
	case OM_SET:
		fi = (struct frinst *)INST(cl, PTR(obj));
		if (hastag(PTR(msg[1]), IA(0x15)))
			fi->recessed = tagdata(PTR(msg[1]), IA(0x15), 0) != 0;
		RET(super_method(cl, obj, msg));
		return;
	case IM_DRAW:
	case IM_DRAWFRAME: {
		struct Image *im = PTR(obj);
		struct RastPort *rp = PTR(msg[1]);
		int x = (int16_t)(msg[2] >> 16) + im->LeftEdge;
		int y = (int16_t)msg[2] + im->TopEdge;
		int w = msg[0] == IM_DRAWFRAME ? (int16_t)(msg[5] >> 16) : im->Width;
		int h = msg[0] == IM_DRAWFRAME ? (int16_t)msg[5] : im->Height;
		int sel = msg[3] == 1 || msg[3] == 6;
		fi = (struct frinst *)INST(cl, im);
		if (!fi->edgesonly)
			gfx_rectfill(rp, x, y, x + w - 1, y + h - 1, sel ?
				     gui_pens[WBP_FILLPEN] : gui_pens[WBP_BACKGROUNDPEN]);
		if (fi->ftype == 2) {           /* FRAME_RIDGE */
			gfx_bevel(rp, x, y, w, h, fi->recessed);
			gfx_bevel(rp, x + 1, y + 1, w - 2, h - 2, !fi->recessed);
		} else
			gfx_bevel(rp, x, y, w, h, fi->recessed ^ sel);
		RET(0);
		return;
	}
	case IM_FRAMEBOX: {
		int16_t *cb = PTR(msg[1]), *fb = PTR(msg[2]);
		fb[0] = cb[0] - 6;
		fb[1] = cb[1] - 2;
		fb[2] = cb[2] + 12;
		fb[3] = cb[3] + 4;
		RET(1);
		return;
	}
	}
	RET(super_method(cl, obj, msg));
}

/* sysiclass: arrows, check marks, mutual exclude buttons */
struct sysinst {
	int which, size;
};

static void cls_sysi(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct sysinst *si;

	switch (msg[0]) {
	case OM_NEW:
		obj = super_method(cl, obj, msg);
		if (obj) {
			struct Image *im = PTR(obj);
			si = (struct sysinst *)INST(cl, im);
			si->which = tagdata(PTR(msg[1]), IA(0x0d), 0);
			si->size = tagdata(PTR(msg[1]), IA(0x0b), 1);
			if (!im->Width || !im->Height) {
				static const uint8_t sizes[18][2] = {
					{ 23, 11 }, { 23, 11 }, { 18, 10 }, { 20, 11 },
					{ 0, 0 }, { 23, 11 }, { 0, 0 }, { 0, 0 },
					{ 0, 0 }, { 0, 0 }, { 16, 10 }, { 18, 11 },
					{ 16, 10 }, { 18, 11 }, { 26, 11 }, { 17, 9 },
					{ 15, 8 }, { 23, 8 } };
				if (si->which < 18) {
					im->Width = sizes[si->which][0];
					im->Height = sizes[si->which][1];
				}
			}
		}
		RET(obj);
		return;
	case IM_DRAW:
	case IM_DRAWFRAME: {
		struct Image *im = PTR(obj);
		struct RastPort *rp = PTR(msg[1]);
		int x = (int16_t)(msg[2] >> 16) + im->LeftEdge;
		int y = (int16_t)msg[2] + im->TopEdge;
		int w = im->Width, h = im->Height, sel = msg[3] == 1 || msg[3] == 6;
		int tp = gui_pens[WBP_TEXTPEN], cx, cy, i;
		si = (struct sysinst *)INST(cl, im);
		gfx_rectfill(rp, x, y, x + w - 1, y + h - 1, sel && si->which != 0x0e &&
			     si->which != 0x0f ? gui_pens[WBP_FILLPEN] :
			     gui_pens[WBP_BACKGROUNDPEN]);
		cx = x + w / 2;
		cy = y + h / 2;
		switch (si->which) {
		case 0x0a: case 0x0b: case 0x0c: case 0x0d:     /* arrows */
			gfx_bevel(rp, x, y, w, h, sel);
			for (i = 0; i < 3; i++)
				switch (si->which) {
				case 0x0a: gfx_rectfill(rp, cx - 1 + i, cy - i, cx - 1 + i, cy + i, tp); break;
				case 0x0c: gfx_rectfill(rp, cx + 1 - i, cy - i, cx + 1 - i, cy + i, tp); break;
				case 0x0b: gfx_rectfill(rp, cx - i, cy - 1 + i, cx + i, cy - 1 + i, tp); break;
				case 0x0d: gfx_rectfill(rp, cx - i, cy + 1 - i, cx + i, cy + 1 - i, tp); break;
				}
			break;
		case 0x0e:                      /* CHECKIMAGE */
			gfx_bevel(rp, x, y, w, h, 0);
			if (sel) {
				gfx_rectfill(rp, cx - 4, cy, cx - 3, cy + 1, tp);
				gfx_rectfill(rp, cx - 2, cy + 1, cx - 1, cy + 2, tp);
				gfx_rectfill(rp, cx, cy - 1, cx + 1, cy, tp);
				gfx_rectfill(rp, cx + 2, cy - 2, cx + 3, cy - 1, tp);
				gfx_rectfill(rp, cx + 4, cy - 3, cx + 5, cy - 2, tp);
			}
			break;
		case 0x0f:                      /* MXIMAGE */
			gfx_bevel(rp, x, y, w, h, sel);
			if (sel)
				gfx_rectfill(rp, x + 3, y + 2, x + w - 4, y + h - 3,
					     gui_pens[WBP_FILLPEN]);
			break;
		default:
			gfx_bevel(rp, x, y, w, h, sel);
		}
		RET(0);
		return;
	}
	}
	RET(super_method(cl, obj, msg));
}

/* fillrectclass: a rectangle in the foreground pen */
static void cls_fillrect(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);

	if (msg[0] == IM_DRAW || msg[0] == IM_DRAWFRAME) {
		struct Image *im = PTR(obj);
		int x = (int16_t)(msg[2] >> 16) + im->LeftEdge;
		int y = (int16_t)msg[2] + im->TopEdge;
		gfx_rectfill(PTR(msg[1]), x, y, x + im->Width - 1, y + im->Height - 1,
			     im->PlanePick);
		RET(0);
		return;
	}
	RET(super_method(cl, obj, msg));
}

/* itexticlass: IA_Data is an IntuiText */
static void cls_itext(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);

	if (msg[0] == IM_DRAW || msg[0] == IM_DRAWFRAME) {
		struct Image *im = PTR(obj);
		if (im->ImageData)
			gfx_printitext(PTR(msg[1]), PTR(im->ImageData),
				       (int16_t)(msg[2] >> 16) + im->LeftEdge,
				       (int16_t)msg[2] + im->TopEdge);
		RET(0);
		return;
	}
	RET(super_method(cl, obj, msg));
}

/* ---- gadgetclass: the instance is a struct Gadget ---- */

struct gadinst {
	struct notify n;
};

static int gadget_set(struct Gadget *g, struct gadinst *gi, struct TagItem *t)
{
	struct TagItem *x;
	int n = 0;

	while ((x = util_next_tag(&t))) {
		uint32_t d = x->ti_Data;
		switch (x->ti_Tag) {
		case GA(1): g->LeftEdge = d; break;
		case GA(2): g->LeftEdge = d; g->Flags |= 0x10; break;
		case GA(3): g->TopEdge = d; break;
		case GA(4): g->TopEdge = d; g->Flags |= 0x08; break;
		case GA(5): g->Width = d; break;
		case GA(6): g->Width = d; g->Flags |= 0x20; break;
		case GA(7): g->Height = d; break;
		case GA(8): g->Height = d; g->Flags |= 0x40; break;
		case GA(9): g->GadgetText = d; g->Flags = (g->Flags & ~0x3000) | 0x1000; break;
		case GA(0x0a): g->GadgetRender = d; g->Flags |= 0x04; break;
		case GA(0x0b): g->GadgetRender = d; g->Flags &= ~0x04; break;
		case GA(0x0c): g->SelectRender = d; g->Flags |= 0x02; break;
		case GA(0x0d): g->Flags = (g->Flags & ~3) | (d & 3); break;
		case GA(0x0e): g->Flags = d ? g->Flags | 0x100 : g->Flags & ~0x100; break;
		case GA(0x10): g->GadgetID = d; break;
		case GA(0x11): g->UserData = d; break;
		case GA(0x12): g->SpecialInfo = d; break;
		case GA(0x13): g->Flags = d ? g->Flags | 0x80 : g->Flags & ~0x80; break;
		case GA(0x14): g->Activation = d ? g->Activation | 4 : g->Activation & ~4; break;
		case GA(0x15): g->Activation = d ? g->Activation | 2 : g->Activation & ~2; break;
		case GA(0x16): g->Activation = d ? g->Activation | 1 : g->Activation & ~1; break;
		case GA(0x17): g->Activation = d ? g->Activation | 8 : g->Activation & ~8; break;
		case GA(0x1c): g->Activation = d ? g->Activation | 0x100 : g->Activation & ~0x100; break;
		case GA(0x1f):                  /* GA_Previous: link after it */
			if (d) {
				struct Gadget *p = PTR(d);
				g->NextGadget = p->NextGadget;
				p->NextGadget = APTR_OF(g);
			}
			break;
		case GA(0x22): g->GadgetText = d; g->Flags &= ~0x3000; break;   /* IntuiText */
		case GA(0x23): g->GadgetText = d; g->Flags = (g->Flags & ~0x3000) | 0x2000; break;
		case ICA_TARGET: gi->n.target = d; break;
		case ICA_MAP: gi->n.map = d; break;
		default: continue;
		}
		n++;
	}
	return n;
}

static void gbox(struct Gadget *g, uint32_t gi, int *x, int *y, int *w, int *h)
{
	struct awin *a = ginfo_win(gi);
	int W = a ? a->w->Width : 0, H = a ? a->w->Height : 0;

	*x = g->LeftEdge + ((g->Flags & 0x10) ? W - 1 : 0);
	*y = g->TopEdge + ((g->Flags & 0x08) ? H - 1 : 0);
	*w = g->Width + ((g->Flags & 0x20) ? W : 0);
	*h = g->Height + ((g->Flags & 0x40) ? H : 0);
}

static void cls_gadget(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct gadinst *gi;

	switch (msg[0]) {
	case OM_NEW:
		obj = super_method(cl, obj, msg);
		if (obj) {
			struct Gadget *g = PTR(obj);
			gi = (struct gadinst *)(INST(cl, g) + 44);
			g->GadgetType = GTYP_CUSTOMGADGET;
			gadget_set(g, gi, PTR(msg[1]));
		}
		RET(obj);
		return;
	case OM_SET:
	case OM_UPDATE:
		gi = (struct gadinst *)(INST(cl, PTR(obj)) + 44);
		RET(gadget_set(PTR(obj), gi, PTR(msg[1])));
		return;
	case OM_GET: {
		struct Gadget *g = PTR(obj);
		uint32_t *st = PTR(msg[2]);
		switch (msg[1]) {
		case GA(0x10): *st = g->GadgetID; break;
		case GA(0x11): *st = g->UserData; break;
		case GA(0x13): *st = (g->Flags & 0x80) != 0; break;
		case GA(0x0e): *st = (g->Flags & 0x100) != 0; break;
		case GA(1): *st = g->LeftEdge; break;
		case GA(3): *st = g->TopEdge; break;
		case GA(5): *st = g->Width; break;
		case GA(7): *st = g->Height; break;
		default: RET(0); return;
		}
		RET(1);
		return;
	}
	case OM_NOTIFY:
		gi = (struct gadinst *)(INST(cl, PTR(obj)) + 44);
		notify(&gi->n, obj, msg[1], msg[2], msg[3]);
		RET(0);
		return;
	case GM_HITTEST:
		RET(GMR_GADGETHIT);
		return;
	case GM_GOACTIVE:
	case GM_HANDLEINPUT:
		RET(GMR_NOREUSE);
		return;
	case GM_RENDER:
	case GM_GOINACTIVE:
	case GM_LAYOUT:
		RET(0);
		return;
	}
	RET(super_method(cl, obj, msg));
}

/* draw a gadget's imagery: GadgetRender (Image or BOOPSI image) and text */
static void render_imagery(struct Gadget *g, struct RastPort *rp, uint32_t gi,
			   int frame)
{
	int x, y, w, h, sel = (g->Flags & 0x80) != 0;
	uint32_t img = sel && g->SelectRender ? g->SelectRender : g->GadgetRender;

	gbox(g, gi, &x, &y, &w, &h);
	if (img && (g->Flags & 0x04)) {
		struct Image *im = PTR(img);
		if (im->Depth == -1) {
			uint32_t *m = msgbuf(6);
			m[0] = frame ? IM_DRAWFRAME : IM_DRAW;
			m[1] = APTR_OF(rp);
			m[2] = (uint32_t)(uint16_t)x << 16 | (uint16_t)y;
			m[3] = (g->Flags & 0x100) ? 2 : sel ? 1 : 0;
			m[4] = APTR_OF(gui_dri);
			m[5] = (uint32_t)(uint16_t)w << 16 | (uint16_t)h;
			do_method(img, m);
		} else
			gfx_drawimage(rp, im, x, y);
	} else if (img)
		gfx_drawborder(rp, PTR(img), x, y);
	if (g->GadgetText) {
		switch (g->Flags & 0x3000) {
		case 0x0000:
			gfx_printitext(rp, PTR(g->GadgetText), x, y);
			break;
		case 0x1000: {                  /* a plain string, centred */
			const char *s = PTR(g->GadgetText);
			int n = strlen(s), tw = gfx_textlength(rp, s, n);
			int8_t fg = rp->FgPen, dm = rp->DrawMode;
			rp->FgPen = sel ? gui_pens[WBP_FILLTEXTPEN] : gui_pens[WBP_TEXTPEN];
			rp->DrawMode = JAM1;
			rp->cp_x = x + (w - tw) / 2;
			rp->cp_y = y + (h - gui_topaz->tf_YSize) / 2 + gui_topaz->tf_Baseline;
			gfx_text(rp, s, n);
			rp->FgPen = fg;
			rp->DrawMode = dm;
			break;
		}
		case 0x2000: {
			struct Image *im = PTR(g->GadgetText);
			if (im->Depth == -1) {
				uint32_t *m = msgbuf(5);
				m[0] = IM_DRAW;
				m[1] = APTR_OF(rp);
				m[2] = (uint32_t)(uint16_t)(x + (w - im->Width) / 2) << 16 |
				       (uint16_t)(y + (h - im->Height) / 2);
				m[3] = sel;
				m[4] = APTR_OF(gui_dri);
				do_method(g->GadgetText, m);
			} else
				gfx_drawimage(rp, im, x, y);
			break;
		}
		}
	}
}

/* buttongclass: press, track, release */
static void cls_button(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct Gadget *g = PTR(obj);

	switch (msg[0]) {
	case GM_RENDER:
		render_imagery(g, PTR(msg[2]), msg[1], 0);
		RET(0);
		return;
	case GM_GOACTIVE: {
		struct awin *a = ginfo_win(msg[1]);
		g->Flags |= 0x80;
		if (a)
			render_imagery(g, a->rp, msg[1], 0);
		RET(GMR_MEACTIVE);
		return;
	}
	case GM_HANDLEINPUT: {
		uint8_t *ie = PTR(msg[2]);
		struct awin *a = ginfo_win(msg[1]);
		int16_t mx = msg[4] >> 16, my = msg[4];
		int x, y, w, h, in, code = *(uint16_t *)(ie + 6);
		gbox(g, msg[1], &x, &y, &w, &h);
		in = mx >= 0 && my >= 0 && mx < w && my < h;
		if (code == 0xe8) {             /* SELECTUP */
			g->Flags &= ~0x80;
			if (a)
				render_imagery(g, a->rp, msg[1], 0);
			if (in) {
				uint32_t *tags = msgbuf(4), *m = msgbuf(4);
				*(int32_t *)PTR(msg[3]) = g->GadgetID;
				tags[0] = GA(0x10);
				tags[1] = g->GadgetID;
				tags[2] = 0;
				m[0] = OM_NOTIFY;
				m[1] = APTR_OF(tags);
				m[2] = msg[1];
				m[3] = 0;
				do_method(obj, m);
				RET(GMR_NOREUSE | GMR_VERIFY);
			} else
				RET(GMR_NOREUSE);
			return;
		}
		if (!!(g->Flags & 0x80) != in) {
			g->Flags ^= 0x80;
			if (a)
				render_imagery(g, a->rp, msg[1], 0);
		}
		RET(GMR_MEACTIVE);
		return;
	}
	case GM_GOINACTIVE: {
		struct awin *a = ginfo_win(msg[1]);
		g->Flags &= ~0x80;
		if (a)
			render_imagery(g, a->rp, msg[1], 0);
		RET(0);
		return;
	}
	}
	RET(super_method(cl, obj, msg));
}

/* frbuttonclass: the image is a frame drawn around the label */
static void cls_frbutton(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct Gadget *g = PTR(obj);

	if (msg[0] == GM_RENDER) {
		render_imagery(g, PTR(msg[2]), msg[1], 1);
		RET(0);
		return;
	}
	if (msg[0] == OM_NEW) {
		obj = super_method(cl, obj, msg);
		if (obj) {
			g = PTR(obj);
			/* size from the frame image when not given */
			if (g->GadgetRender && (g->Flags & 0x04) && !g->Width) {
				struct Image *im = PTR(g->GadgetRender);
				g->Width = im->Width;
				g->Height = im->Height;
			}
		}
		RET(obj);
		return;
	}
	RET(super_method(cl, obj, msg));
}

/* propgclass: a proportional gadget (PropInfo kept in the instance) */
struct propinst {
	struct PropInfo pi;
	uint32_t total, visible, top;
	int drag, dragoff;
};

static void prop_from_tvt(struct propinst *p)
{
	uint32_t hid = p->total > p->visible ? p->total - p->visible : 0;
	uint16_t body = p->total ? (p->visible >= p->total ? 0xffff :
		       (uint32_t)p->visible * 0xffff / p->total) : 0xffff;
	uint16_t pot = hid ? (uint32_t)(p->top > hid ? hid : p->top) * 0xffff / hid : 0;

	if (p->pi.Flags & 4) {
		p->pi.VertBody = body;
		p->pi.VertPot = pot;
	} else {
		p->pi.HorizBody = body;
		p->pi.HorizPot = pot;
	}
}

static int prop_set(struct propinst *p, struct TagItem *t)
{
	struct TagItem *x;
	int n = 0, tvt = 0;

	while ((x = util_next_tag(&t))) {
		uint32_t d = x->ti_Data;
		switch (x->ti_Tag) {
		case PGA(1): p->pi.Flags = (p->pi.Flags & ~6) | (d == 2 ? 4 : 2); break;
		case PGA(2): if (d) p->pi.Flags |= 8; break;
		case PGA(3): p->pi.HorizPot = d; break;
		case PGA(4): p->pi.HorizBody = d; break;
		case PGA(5): p->pi.VertPot = d; break;
		case PGA(6): p->pi.VertBody = d; break;
		case PGA(7): p->total = d; tvt = 1; break;
		case PGA(8): p->visible = d; tvt = 1; break;
		case PGA(9): p->top = d; tvt = 1; break;
		default: continue;
		}
		n++;
	}
	if (tvt)
		prop_from_tvt(p);
	return n;
}

static void cls_prop(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct Gadget *g = PTR(obj);
	struct propinst *p = obj ? (struct propinst *)INST(cl, g) : NULL;

	switch (msg[0]) {
	case OM_NEW:
		obj = super_method(cl, obj, msg);
		if (obj) {
			g = PTR(obj);
			p = (struct propinst *)INST(cl, g);
			p->pi.Flags = 1 | 2 | 0x10;     /* AUTOKNOB FREEHORIZ NEWLOOK */
			p->pi.HorizBody = p->pi.VertBody = 0xffff;
			p->total = p->visible = 1;
			prop_set(p, PTR(msg[1]));
			g->SpecialInfo = APTR_OF(&p->pi);
		}
		RET(obj);
		return;
	case OM_SET:
	case OM_UPDATE: {
		int n = prop_set(p, PTR(msg[1]));
		RET(super_method(cl, obj, msg) + n);
		return;
	}
	case OM_GET: {
		uint32_t *st = PTR(msg[2]);
		switch (msg[1]) {
		case PGA(9): *st = p->top; break;
		case PGA(7): *st = p->total; break;
		case PGA(8): *st = p->visible; break;
		case PGA(3): *st = p->pi.HorizPot; break;
		case PGA(5): *st = p->pi.VertPot; break;
		default: RET(super_method(cl, obj, msg)); return;
		}
		RET(1);
		return;
	}
	case GM_RENDER: {
		struct RastPort *rp = PTR(msg[2]);
		int x, y, w, h, kx, ky, kw, kh;
		gbox(g, msg[1], &x, &y, &w, &h);
		if (!(p->pi.Flags & 8))
			gfx_bevel(rp, x, y, w, h, 0);
		gfx_rectfill(rp, x + 2, y + 1, x + w - 3, y + h - 2,
			     gui_pens[WBP_BACKGROUNDPEN]);
		kw = (p->pi.Flags & 2) ? (w - 4) * p->pi.HorizBody / 0xffff : w - 4;
		kh = (p->pi.Flags & 4) ? (h - 2) * p->pi.VertBody / 0xffff : h - 2;
		if (kw < 4) kw = 4;
		if (kh < 4) kh = 4;
		kx = x + 2 + ((p->pi.Flags & 2) ? (w - 4 - kw) * p->pi.HorizPot / 0xffff : 0);
		ky = y + 1 + ((p->pi.Flags & 4) ? (h - 2 - kh) * p->pi.VertPot / 0xffff : 0);
		gfx_rectfill(rp, kx, ky, kx + kw - 1, ky + kh - 1, gui_pens[WBP_FILLPEN]);
		gfx_bevel(rp, kx, ky, kw, kh, 0);
		RET(0);
		return;
	}
	case GM_GOACTIVE:
	case GM_HANDLEINPUT: {
		struct awin *a = ginfo_win(msg[1]);
		uint8_t *ie = PTR(msg[2]);
		int16_t mx = msg[4] >> 16, my = msg[4];
		int x, y, w, h, code = ie ? *(uint16_t *)(ie + 6) : 0x68;
		uint32_t hid = p->total > p->visible ? p->total - p->visible : 0;
		int vert = (p->pi.Flags & 4) != 0;
		int len = vert ? h : w, pos = vert ? my : mx;
		gbox(g, msg[1], &x, &y, &w, &h);
		len = vert ? h - 2 : w - 4;
		pos = vert ? my - 1 : mx - 2;
		if (len > 0 && hid) {
			long t = (long)pos * (long)(hid + 1) / len;
			p->top = t < 0 ? 0 : (uint32_t)t > hid ? hid : t;
			prop_from_tvt(p);
		}
		if (a) {
			uint32_t *m = msgbuf(4), *tags = msgbuf(4);
			m[0] = GM_RENDER;
			m[1] = msg[1];
			m[2] = APTR_OF(a->rp);
			m[3] = 1;
			do_method(obj, m);
			tags[0] = PGA(9);
			tags[1] = p->top;
			tags[2] = 0;
			m[0] = OM_NOTIFY;
			m[1] = APTR_OF(tags);
			m[2] = msg[1];
			m[3] = code == 0xe8 ? 0 : 1;    /* OPUF_INTERIM while dragging */
			do_method(obj, m);
		}
		if (code == 0xe8) {
			*(int32_t *)PTR(msg[3]) = p->top;
			RET(GMR_NOREUSE | GMR_VERIFY);
		} else
			RET(GMR_MEACTIVE);
		return;
	}
	}
	RET(super_method(cl, obj, msg));
}

/* strgclass: a string gadget; Intuition's string editing does the work */
struct strinst {
	struct StringInfo si;
};

static void cls_str(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct Gadget *g = PTR(obj);

	switch (msg[0]) {
	case OM_NEW:
		obj = super_method(cl, obj, msg);
		if (obj) {
			struct strinst *s;
			struct TagItem *t = PTR(msg[1]);
			int max = tagdata(t, STRINGA(1), 128);
			g = PTR(obj);
			s = (struct strinst *)INST(cl, g);
			s->si.MaxChars = max;
			s->si.Buffer = tagdata(t, STRINGA(2), 0);
			if (!s->si.Buffer)
				s->si.Buffer = APTR_OF(amem_alloc(max + 1));
			if (hastag(t, STRINGA(0x12)) && tagdata(t, STRINGA(0x12), 0))
				snprintf(PTR(s->si.Buffer), max, "%s",
					 (char *)PTR(tagdata(t, STRINGA(0x12), 0)));
			if (hastag(t, STRINGA(0x11))) {
				g->Activation |= 0x800;
				snprintf(PTR(s->si.Buffer), max, "%d",
					 (int)tagdata(t, STRINGA(0x11), 0));
			}
			g->SpecialInfo = APTR_OF(&s->si);
			g->GadgetType = 4;      /* Intuition's string gadget */
			g->Activation |= 1;
		}
		RET(obj);
		return;
	case OM_GET: {
		struct StringInfo *si = PTR(g->SpecialInfo);
		uint32_t *st = PTR(msg[2]);
		if (msg[1] == STRINGA(0x12)) {
			*st = si->Buffer;
			RET(1);
			return;
		}
		if (msg[1] == STRINGA(0x11)) {
			*st = strtol(PTR(si->Buffer), NULL, 10);
			RET(1);
			return;
		}
		break;
	}
	case OM_SET:
	case OM_UPDATE: {
		struct StringInfo *si = PTR(g->SpecialInfo);
		struct TagItem *t = PTR(msg[1]);
		if (hastag(t, STRINGA(0x12)))
			snprintf(PTR(si->Buffer), si->MaxChars, "%s",
				 tagdata(t, STRINGA(0x12), 0) ?
				 (char *)PTR(tagdata(t, STRINGA(0x12), 0)) : "");
		if (hastag(t, STRINGA(0x11)))
			snprintf(PTR(si->Buffer), si->MaxChars, "%d",
				 (int)tagdata(t, STRINGA(0x11), 0));
		break;
	}
	}
	RET(super_method(cl, obj, msg));
}

/* icclass: passes OM_UPDATEs on through ICA_MAP to ICA_TARGET */
struct icinst {
	struct notify n;
};

static void cls_ic(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct icinst *ic;

	switch (msg[0]) {
	case OM_NEW:
		obj = super_method(cl, obj, msg);
		if (obj) {
			ic = (struct icinst *)INST(cl, PTR(obj));
			ic->n.target = tagdata(PTR(msg[1]), ICA_TARGET, 0);
			ic->n.map = tagdata(PTR(msg[1]), ICA_MAP, 0);
		}
		RET(obj);
		return;
	case OM_SET:
		ic = (struct icinst *)INST(cl, PTR(obj));
		if (hastag(PTR(msg[1]), ICA_TARGET))
			ic->n.target = tagdata(PTR(msg[1]), ICA_TARGET, 0);
		if (hastag(PTR(msg[1]), ICA_MAP))
			ic->n.map = tagdata(PTR(msg[1]), ICA_MAP, 0);
		RET(1);
		return;
	case OM_UPDATE:
	case OM_NOTIFY:
		ic = (struct icinst *)INST(cl, PTR(obj));
		notify(&ic->n, obj, msg[1], msg[2], msg[3]);
		RET(0);
		return;
	}
	RET(super_method(cl, obj, msg));
}

/* modelclass: an icclass that also tells every member */
struct modelinst {
	uint32_t members[32];
	int nmembers;
};

static void cls_model(struct frame *f)
{
	struct IClass *cl = PTR(A(0));
	uint32_t *msg = PTR(A(1)), obj = A(2);
	struct modelinst *mi = obj && msg[0] != OM_NEW ?
			       (struct modelinst *)INST(cl, PTR(obj)) : NULL;
	int i;

	switch (msg[0]) {
	case OM_ADDMEMBER:
		if (mi->nmembers < 32)
			mi->members[mi->nmembers++] = msg[1];
		RET(0);
		return;
	case OM_REMMEMBER:
		for (i = 0; i < mi->nmembers; i++)
			if (mi->members[i] == msg[1])
				mi->members[i] = mi->members[--mi->nmembers];
		RET(0);
		return;
	case OM_DISPOSE:
		for (i = 0; i < mi->nmembers; i++) {
			uint32_t *m = msgbuf(1);
			m[0] = OM_DISPOSE;
			do_method(mi->members[i], m);
		}
		break;
	case OM_UPDATE:
	case OM_NOTIFY:
		for (i = 0; i < mi->nmembers; i++) {
			uint32_t *m = msgbuf(4);
			m[0] = OM_UPDATE;
			m[1] = msg[1];
			m[2] = msg[2];
			m[3] = msg[3];
			do_method(mi->members[i], m);
		}
		break;
	}
	RET(super_method(cl, obj, msg));
}

/* ---- the hidden library whose vectors are the system dispatchers ---- */

static const struct fdent fd_boopsi[] = {
	{ 30, "rootclass", "a0/a1/a2" }, { 36, "imageclass", "a0/a1/a2" },
	{ 42, "frameiclass", "a0/a1/a2" }, { 48, "sysiclass", "a0/a1/a2" },
	{ 54, "fillrectclass", "a0/a1/a2" }, { 60, "itexticlass", "a0/a1/a2" },
	{ 66, "gadgetclass", "a0/a1/a2" }, { 72, "buttongclass", "a0/a1/a2" },
	{ 78, "frbuttonclass", "a0/a1/a2" }, { 84, "propgclass", "a0/a1/a2" },
	{ 90, "strgclass", "a0/a1/a2" }, { 96, "icclass", "a0/a1/a2" },
	{ 102, "modelclass", "a0/a1/a2" },
	{ 0, 0, 0 }
};

static const struct libimpl boopsi_impl[] = {
	{ "rootclass", cls_root }, { "imageclass", cls_image },
	{ "frameiclass", cls_frame }, { "sysiclass", cls_sysi },
	{ "fillrectclass", cls_fillrect }, { "itexticlass", cls_itext },
	{ "gadgetclass", cls_gadget }, { "buttongclass", cls_button },
	{ "frbuttonclass", cls_frbutton }, { "propgclass", cls_prop },
	{ "strgclass", cls_str }, { "icclass", cls_ic },
	{ "modelclass", cls_model },
	{ NULL, NULL }
};

struct amilib lib_boopsi = {
	.name = "amigafmt boopsi", .version = 40, .revision = 1,
	.fd = fd_boopsi, .impl = boopsi_impl, .possize = 8, .device = 1,
	.hidden = 1,
};

static struct IClass *make_class(const char *id, struct IClass *super,
				 uint16_t isize)
{
	struct IClass *cl = amem_alloc(sizeof(*cl));
	char *name = NULL;

	if (id) {
		name = amem_alloc(strlen(id) + 1);
		strcpy(name, id);
	}
	cl->cl_ID = APTR_OF(name);
	cl->cl_Super = APTR_OF(super);
	if (super) {
		cl->cl_InstOffset = super->cl_InstOffset + super->cl_InstSize;
		super->cl_SubclassCount++;
	}
	cl->cl_InstSize = isize;
	return cl;
}

static void add_public(struct IClass *cl)
{
	struct pubclass *p = calloc(1, sizeof(*p));

	snprintf(p->name, sizeof(p->name), "%s", (char *)PTR(cl->cl_ID));
	p->cl = cl;
	cl->cl_Flags |= 1;                      /* CLF_INLIST */
	p->next = pubs;
	pubs = p;
}

static struct IClass *find_public(const char *name)
{
	struct pubclass *p;

	for (p = pubs; p; p = p->next)
		if (!strcmp(p->name, name))
			return p->cl;
	return NULL;
}

static void init_classes(void)
{
	static int done;
	struct Library *b;
	struct { const char *name, *super; int slot; uint16_t isize; } sys[] = {
		{ "rootclass", NULL, 1, 0 },
		{ "imageclass", "rootclass", 2, 20 },
		{ "frameiclass", "imageclass", 3, sizeof(struct frinst) },
		{ "sysiclass", "imageclass", 4, sizeof(struct sysinst) },
		{ "fillrectclass", "imageclass", 5, 4 },
		{ "itexticlass", "imageclass", 6, 4 },
		{ "gadgetclass", "rootclass", 7, 44 + sizeof(struct gadinst) },
		{ "buttongclass", "gadgetclass", 8, 4 },
		{ "frbuttonclass", "buttongclass", 9, 4 },
		{ "propgclass", "gadgetclass", 10, sizeof(struct propinst) },
		{ "strgclass", "gadgetclass", 11, sizeof(struct strinst) },
		{ "icclass", "rootclass", 12, sizeof(struct icinst) },
		{ "modelclass", "icclass", 13, sizeof(struct modelinst) },
	};
	int i;

	if (done)
		return;
	done = 1;
	b = lib_build(&lib_boopsi);
	for (i = 0; i < (int)(sizeof(sys) / sizeof(sys[0])); i++) {
		struct IClass *super = sys[i].super ? find_public(sys[i].super) : NULL;
		struct IClass *cl = make_class(sys[i].name, super, sys[i].isize);
		/* the instance of imageclass/gadgetclass IS the Image/Gadget:
		   it starts at the object pointer */
		cl->cl_Dispatcher.h_Entry = APTR_OF((uint8_t *)b - 6 * (4 + sys[i].slot));
		add_public(cl);
	}
	/* gadgetclass's and imageclass's sizes include the structure */
	find_public("imageclass")->cl_InstOffset = 0;
	find_public("gadgetclass")->cl_InstOffset = 0;
	for (i = 0; i < (int)(sizeof(sys) / sizeof(sys[0])); i++) {
		struct IClass *cl = find_public(sys[i].name);
		struct IClass *super = PTR(cl->cl_Super);
		if (super)
			cl->cl_InstOffset = super->cl_InstOffset + super->cl_InstSize;
	}
}

/* ---- intuition's entry points ---- */

static void i_MakeClass(struct frame *f)
{
	struct IClass *super = A(2) ? PTR(A(2)) : NULL;

	init_classes();
	if (!super && A(1))
		super = find_public(PTR(A(1)));
	if (!super && (A(1) || A(2))) {
		RET(0);
		return;
	}
	RET(make_class(A(0) ? PTR(A(0)) : NULL, super, D(0)));
}

static void i_AddClass(struct frame *f)
{
	if (A(0) && ((struct IClass *)PTR(A(0)))->cl_ID)
		add_public(PTR(A(0)));
}

static void i_RemoveClass(struct frame *f)
{
	struct pubclass **pp, *p;

	for (pp = &pubs; (p = *pp); pp = &p->next)
		if (APTR_OF(p->cl) == A(0)) {
			*pp = p->next;
			free(p);
			break;
		}
}

static void i_FreeClass(struct frame *f)
{
	struct IClass *cl = PTR(A(0));

	if (!cl || cl->cl_ObjectCount || cl->cl_SubclassCount) {
		RET(0);
		return;
	}
	i_RemoveClass(f);
	if (cl->cl_Super)
		((struct IClass *)PTR(cl->cl_Super))->cl_SubclassCount--;
	RET(1);
}

static void i_NewObjectA(struct frame *f)
{
	struct IClass *cl = A(0) ? PTR(A(0)) : NULL;
	uint32_t *m;

	init_classes();
	if (!cl && A(1))
		cl = find_public(PTR(A(1)));
	if (!cl) {
		if (af_trace)
			af_warn("NewObject: no class \"%s\"",
				A(1) ? (char *)PTR(A(1)) : "?");
		RET(0);
		return;
	}
	m = msgbuf(3);
	m[0] = OM_NEW;
	m[1] = A(2);
	m[2] = 0;
	RET(boopsi_do(cl, APTR_OF(cl), m));
}

static void i_DisposeObject(struct frame *f)
{
	uint32_t *m = msgbuf(1);

	if (!A(0))
		return;
	m[0] = OM_DISPOSE;
	do_method(A(0), m);
}

static void i_SetAttrsA(struct frame *f)
{
	uint32_t *m = msgbuf(3);

	m[0] = OM_SET;
	m[1] = A(1);
	m[2] = 0;
	RET(do_method(A(0), m));
}

static void i_GetAttr(struct frame *f)
{
	uint32_t *m = msgbuf(3);

	m[0] = OM_GET;
	m[1] = D(0);
	m[2] = A(1);
	RET(do_method(A(0), m));
}

static void render_custom(struct awin *a, uint32_t g, int redraw)
{
	uint32_t *m = msgbuf(4);

	m[0] = GM_RENDER;
	m[1] = APTR_OF(ginfo(a));
	m[2] = APTR_OF(a->rp);
	m[3] = redraw;
	do_method(g, m);
}

static void i_SetGadgetAttrsA(struct frame *f)
{
	struct awin *a = A(1) ? gui_by_window(A(1)) : NULL;
	uint32_t *m = msgbuf(3), r;

	m[0] = OM_SET;
	m[1] = A(3);
	m[2] = a ? APTR_OF(ginfo(a)) : 0;
	r = do_method(A(0), m);
	if (r && a)
		render_custom(a, A(0), 2);
	RET(r);
}

static void i_DoGadgetMethodA(struct frame *f)
{
	struct awin *a = A(1) ? gui_by_window(A(1)) : NULL;
	uint32_t *msg = PTR(A(3));

	/* methods with a GadgetInfo in their second long get one */
	if (a && (msg[0] < 0x100 || msg[0] == OM_SET || msg[0] == OM_UPDATE ||
		  msg[0] == OM_NOTIFY))
		msg[msg[0] < 0x100 ? 1 : 2] = APTR_OF(ginfo(a));
	RET(do_method(A(0), msg));
}

static void i_ObtainGIRPort(struct frame *f)
{
	RET(A(0) ? *(uint32_t *)((uint8_t *)PTR(A(0)) + 12) : 0);
}

static void i_Nop(struct frame *f) { (void)f; }

static void i_NextObject(struct frame *f)
{
	uint32_t *pp = PTR(A(0));
	struct MinNode *n = PTR(*pp);

	if (!n || !n->mln_Succ) {
		RET(0);
		return;
	}
	*pp = n->mln_Succ;
	RET((uint32_t *)n + 3);
}

const struct libimpl boopsi_intuition_impl[] = {
	{ "MakeClass", i_MakeClass }, { "AddClass", i_AddClass },
	{ "RemoveClass", i_RemoveClass }, { "FreeClass", i_FreeClass },
	{ "NewObjectA", i_NewObjectA }, { "DisposeObject", i_DisposeObject },
	{ "SetAttrsA", i_SetAttrsA }, { "GetAttr", i_GetAttr },
	{ "SetGadgetAttrsA", i_SetGadgetAttrsA },
	{ "DoGadgetMethodA", i_DoGadgetMethodA },
	{ "ObtainGIRPort", i_ObtainGIRPort }, { "ReleaseGIRPort", i_Nop },
	{ "NextObject", i_NextObject },
	{ NULL, NULL }
};

/* ---- custom gadgets inside Intuition's input handling ---- */

int boopsi_is_custom(struct Gadget *g)
{
	return (g->GadgetType & 7) == GTYP_CUSTOMGADGET;
}

void boopsi_render(struct awin *a, struct Gadget *g)
{
	render_custom(a, APTR_OF(g), 1);
}

int boopsi_hittest(struct awin *a, struct Gadget *g, int rx, int ry)
{
	uint32_t *m = msgbuf(3);

	m[0] = GM_HITTEST;
	m[1] = APTR_OF(ginfo(a));
	m[2] = (uint32_t)(uint16_t)rx << 16 | (uint16_t)ry;
	return do_method(APTR_OF(g), m) == GMR_GADGETHIT;
}

/* GM_GOACTIVE / GM_HANDLEINPUT: returns 1 while the gadget stays active */
int boopsi_input(struct awin *a, struct Gadget *g, int first, int code,
		 int qual, int rx, int ry)
{
	uint32_t *m = msgbuf(6), *term = msgbuf(1), r;
	uint8_t *ie = (uint8_t *)msgbuf(6);

	memset(ie, 0, 22);
	ie[4] = 2;                              /* IECLASS_RAWMOUSE */
	*(uint16_t *)(ie + 6) = code;
	*(uint16_t *)(ie + 8) = qual;
	*term = 0;
	m[0] = first ? GM_GOACTIVE : GM_HANDLEINPUT;
	m[1] = APTR_OF(ginfo(a));
	m[2] = APTR_OF(ie);
	m[3] = APTR_OF(term);
	m[4] = (uint32_t)(uint16_t)rx << 16 | (uint16_t)ry;
	m[5] = 0;
	r = do_method(APTR_OF(g), m);
	if (r == GMR_MEACTIVE)
		return 1;
	if (r & GMR_VERIFY)
		intui_put_imsg(a, IDCMP_GADGETUP, *term, qual, APTR_OF(g),
			       a->w->MouseX, a->w->MouseY);
	m[0] = GM_GOINACTIVE;
	m[1] = APTR_OF(ginfo(a));
	m[2] = 0;
	do_method(APTR_OF(g), m);
	return 0;
}

/* DrawImage on a BOOPSI image (Depth == CUSTOMIMAGEDEPTH) */
void boopsi_drawimage(struct RastPort *rp, struct Image *im, int x, int y,
		      int state)
{
	uint32_t *m = msgbuf(5);

	m[0] = IM_DRAW;
	m[1] = APTR_OF(rp);
	m[2] = (uint32_t)(uint16_t)x << 16 | (uint16_t)y;
	m[3] = state;
	m[4] = APTR_OF(gui_dri);
	do_method(APTR_OF(im), m);
}
