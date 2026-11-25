/* amigafmt's display: wbde's client protocol, shared by intuition,
   graphics and gadtools */
#ifndef GUI_H
#define GUI_H

#include "af.h"
#include "wbde_proto.h"

/* one Intuition window: the Window structure and wbde's window behind it */
struct awin {
	struct Window *w;
	struct RastPort *rp;
	struct BitMap *bm;
	uint32_t id;                    /* wbde's */
	uint8_t *pix;                   /* the inner area, pens */
	int iw, ih, stride;
	size_t size;
	int bl, bt, br, bb;             /* borders: window coords = inner + (bl,bt) */
	int dirty, dx0, dy0, dx1, dy1;  /* inner coords, inclusive */
	int raw_pending;                /* a RAWKEY waiting to see if a VANILLAKEY follows */
	struct wbp_event raw;
	struct Gadget *agad;            /* gadget being held down */
	int agad_in;
	struct Gadget *strgad;          /* active string gadget */
	uint32_t lastsel_s, lastsel_us;
	int easy;                       /* an EasyRequest window of ours */
	int easy_result;
	struct awin *next;
};

extern struct awin *gui_wins;
extern struct wbp_screen gui_screen;
extern struct Screen *wb_screen;
extern struct TextFont *gui_topaz;
extern struct TextAttr *gui_topaz_attr;
extern struct DrawInfo *gui_dri;
extern uint16_t gui_pens[16];
extern struct Library *GfxBase;

int gui_connect(void);
int gui_fd(void);
struct awin *gui_open(const struct wbp_open *o);
void gui_close(struct awin *a);
void gui_send(uint32_t type, uint32_t win, const void *p, uint32_t len);
void gui_damage(struct awin *a, int x0, int y0, int x1, int y1);
void gui_flush(void);
int gui_pump(int timeout_ms);
int gui_obtain_pen(uint8_t r, uint8_t g, uint8_t b);
struct awin *gui_by_window(APTR w);
struct awin *gui_by_rp(APTR rp);

/* intuition.c */
void intui_event(struct awin *a, const struct wbp_event *e);
void intui_resized(struct awin *a);
void intui_render_gadgets(struct awin *a, struct Gadget *g, int n);
struct awin *intui_open_plain(const char *title, int iw, int ih);
void asl_event(struct awin *a, const struct wbp_event *e);

/* gfx.c: drawing on a RastPort */
void gfx_rectfill(struct RastPort *rp, int x0, int y0, int x1, int y1, int pen);
void gfx_text(struct RastPort *rp, const char *s, int n);
int gfx_textlength(struct RastPort *rp, const char *s, int n);
void gfx_line(struct RastPort *rp, int x0, int y0, int x1, int y1);
void gfx_pixel(struct RastPort *rp, int x, int y, int pen);
void gfx_complement(struct RastPort *rp, int x0, int y0, int x1, int y1);
void gfx_drawimage(struct RastPort *rp, struct Image *im, int x, int y);
void gfx_drawborder(struct RastPort *rp, struct Border *b, int x, int y);
void gfx_printitext(struct RastPort *rp, struct IntuiText *it, int x, int y);
int gfx_itextlength(struct IntuiText *it);
void gfx_init_rp(struct RastPort *rp);
void gfx_bevel(struct RastPort *rp, int x, int y, int w, int h, int recessed);
struct TextFont *gfx_font_for(struct TextAttr *ta);

/* boopsi.c */
int boopsi_is_custom(struct Gadget *g);
void boopsi_render(struct awin *a, struct Gadget *g);
int boopsi_hittest(struct awin *a, struct Gadget *g, int rx, int ry);
int boopsi_input(struct awin *a, struct Gadget *g, int first, int code,
		 int qual, int rx, int ry);
void boopsi_drawimage(struct RastPort *rp, struct Image *im, int x, int y,
		      int state);

/* gadtools.c: its gadgets inside Intuition's input handling */
int gt_handle_down(struct awin *a, struct Gadget *g, int x, int y);
int gt_handle_up(struct awin *a, struct Gadget *g, int x, int y, int inside);
int gt_handle_move(struct awin *a, struct Gadget *g, int x, int y);
int gt_render(struct awin *a, struct Gadget *g);
int gt_is_ours(struct Gadget *g);
int gt_key(struct awin *a, struct Gadget *g, int ch, int raw, int qual);

#endif
