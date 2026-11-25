/* amigafmt's display: wbde's client protocol, shared by intuition,
   graphics and gadtools */
#ifndef GUI_H
#define GUI_H

#include "af.h"
#include "wbde_proto.h"

/* one Intuition window: the Window structure and wbde's window behind it */
struct awin {
	struct Window *w;
	struct ascreen *as;             /* its custom screen, NULL on Workbench */
	int root;                       /* a backdrop window that IS the screen's
					   pixels: no wbde window, drawn in the
					   screen's buffer through its Layer */
	int root_active, root_rmb;
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
	void *aslreq;                   /* a file requester of ours */
	struct awin *next;
};

/* a custom screen: wbde's screen behind a Screen structure (screen.c) */
struct ascreen {
	struct Screen *scr;
	uint32_t id;                    /* wbde's */
	uint8_t *pix;                   /* the screen's own pixels, pens */
	int w, h, stride;
	size_t size;
	int dirty, dx0, dy0, dx1, dy1;
	int depth;
	uint32_t modeid;
	struct DrawInfo *dri;
	uint16_t *pens;
	struct TextFont *font;
	uint8_t pal[256][3];
	int pal_lo, pal_hi;             /* colours to send, -1 none */
	uint8_t *cm;                    /* the ViewPort's ColorMap */
	int barh, wbortop;
	int pub, publocks;
	char pubname[64];
	/* the Screen's BitMap: real planes, the screen's pixels, write
	   protected so that whatever writes them (graphics, or the program
	   itself) marks the rows to convert into pix at the next flush */
	uint8_t *planes[8];
	uint8_t *alias[8];              /* the same memory, always writable: amigafmt's own drawing */
	size_t planesize;
	int bpr;
	volatile int conv_y0, conv_y1;  /* rows to convert, y0 > y1: none */
	struct ascreen *next;           /* front to back */
};
int screen_plane_fault(uint32_t addr);
void as_rows_drawn(struct ascreen *as, int y0, int y1);
extern struct ascreen *gui_screens;
struct ascreen *as_by_screen(uint32_t scr);
struct ascreen *as_by_rp(struct RastPort *rp);
void as_damage(struct ascreen *as, int x0, int y0, int x1, int y1);
void as_flush(void);
void as_set_title(struct ascreen *as, const char *title, int show);
void mode_size(uint32_t id, int *w, int *h);
int mode_known(uint32_t id);

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
int gui_call(uint32_t type, uint32_t win, const void *p, uint32_t len, uint32_t reply,
	     void *out, size_t outlen, int *outfd, uint32_t *outwin);
void *gui_map(int fd, size_t size);
void gui_unmap(void *p, size_t size);
struct awin *gui_by_window(APTR w);
struct awin *gui_by_rp(APTR rp);
void gui_add_root(struct awin *a);

/* intuition.c */
void intui_event(struct awin *a, const struct wbp_event *e);
void intui_resized(struct awin *a);
void intui_render_gadgets(struct awin *a, struct Gadget *g, int n);
struct awin *intui_open_plain(uint32_t screen, const char *title, int iw, int ih);
int intui_pen(struct awin *a, int pen);
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
