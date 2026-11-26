/*
 * asl.library: file, font and screen-mode requesters drawn by amigafmt in
 * a window of its own, on the screen of the window they belong to
 * (ASLxx_Window), or ASLxx_Screen / ASLxx_PubScreenName, else Workbench.
 *
 * All three are a list with a scroller, a line or two of text fields and
 * a row of buttons.  File: drawers first (marked), a click on a drawer
 * enters it, on a file picks it, a double click is OK; the drawer and the
 * file name are typed into (Tab switches), Return is OK, Esc Cancel.
 * Font: the font names (AvailFonts: in memory and FONTS:), and the sizes
 * of the one picked.  Screen mode: the display database's modes, with the
 * depth chosen by - and +.
 *
 * Offsets are the 3.1 NDK asl_lib.fd's (tools/fdcheck.py), structures and
 * tags libraries/asl.h's.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "gui.h"

#define ASL_TB 0x80080000
#define ASL_FileRequest 0
#define ASL_FontRequest 1
#define ASL_ScreenModeRequest 2
#define ROWS 10
#define LW 296                  /* list width */

/* our part of a requester, after the public structure */
#define REQ_SIZE   512
#define REQ_TYPE   400          /* uint8 */
#define REQ_TAGS   404          /* the tags given to AllocAslRequest */
#define REQ_NAME   408          /* the font name buffer */

extern uint32_t gfx_availfonts(uint8_t *buf, int32_t size, uint32_t flags);
extern uint32_t gfx_next_mode(uint32_t id);
extern int gfx_mode_info(uint32_t id, uint8_t *name, uint8_t *dims);
extern int pat_matchraw(const char *pat, const char *str, int nocase);
extern int32_t pat_parse(const char *src, char *dst, int32_t len, int nc);

struct item {
	char *name;
	int kind;               /* file: 1 drawer; font: size; mode: index */
	uint32_t id;
};

struct asl {
	int type;
	struct awin *a;
	char drawer[256], file[128], pattern[128];
	char tok[260];          /* parsed pattern */
	int rejecticons, drawersonly;
	char accept[260];       /* a parsed ASLFR_AcceptPattern, or "" */
	struct item *items;
	int n, top, sel, result, field;
	int64_t lastclick;
	int lastrow;
	/* font */
	char fontname[64];
	int fontsize;
	struct item *sizes;
	int nsizes;
	/* screen mode */
	uint32_t modeid;
	int depth, mindepth, maxdepth;
	/* layout, inner coordinates */
	int listy, fy, by, bh, nb, bx[4], bw[4];
	const char *lab[4];
};

static int fh(void) { return gui_topaz->tf_YSize; }
static int cw(void) { return gui_topaz->tf_XSize; }

static void text(struct awin *a, int x, int y, const char *s, int pen, int max)
{
	struct RastPort *rp = a->rp;
	int n = strlen(s);

	if (max && n > max)
		n = max;
	rp->FgPen = pen;
	rp->DrawMode = JAM1;
	rp->cp_x = a->bl + x;
	rp->cp_y = a->bt + y + gui_topaz->tf_Baseline;
	gfx_text(rp, s, n);
}

static int icmp(const void *x, const void *y)
{
	const struct item *a = x, *b = y;

	if (a->kind != b->kind && (a->kind == 1 || b->kind == 1))
		return b->kind - a->kind;       /* drawers first */
	return strcasecmp(a->name, b->name);
}

static void free_items(struct item **v, int *n)
{
	int i;

	for (i = 0; i < *n; i++)
		free((*v)[i].name);
	free(*v);
	*v = NULL;
	*n = 0;
}

static void add_item(struct item **v, int *n, const char *name, int kind, uint32_t id)
{
	*v = realloc(*v, (*n + 1) * sizeof(**v));
	(*v)[*n].name = strdup(name);
	(*v)[*n].kind = kind;
	(*v)[*n].id = id;
	(*n)++;
}

/* ---- the three kinds' lists ---- */

static void scan_files(struct asl *q)
{
	char *lp = path_to_linux(q->drawer, NULL), full[1200];
	DIR *d = lp ? opendir(lp) : NULL;
	struct dirent *e;

	free_items(&q->items, &q->n);
	q->top = 0;
	q->sel = -1;
	while (d && (e = readdir(d))) {
		struct stat st;
		size_t l = strlen(e->d_name);
		if (e->d_name[0] == '.')
			continue;
		snprintf(full, sizeof(full), "%s/%s", lp, e->d_name);
		if (stat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			add_item(&q->items, &q->n, e->d_name, 1, 0);
			continue;
		}
		if (q->drawersonly)
			continue;
		if (q->rejecticons && l > 5 && !strcasecmp(e->d_name + l - 5, ".info"))
			continue;
		if (q->accept[0] && !pat_matchraw(q->accept, e->d_name, 1))
			continue;
		if (q->tok[0] && !pat_matchraw(q->tok, e->d_name, 1))
			continue;
		add_item(&q->items, &q->n, e->d_name, 0, 0);
	}
	if (d)
		closedir(d);
	free(lp);
	if (q->n)
		qsort(q->items, q->n, sizeof(*q->items), icmp);
}

/* every font AvailFonts knows: unique names, and the sizes of the one picked */
static void scan_fonts(struct asl *q)
{
	uint8_t *buf;
	int32_t size = 4096, more;
	int i, n;

	buf = malloc(size);
	while ((more = gfx_availfonts(buf, size, 3)) > 0) {
		size += more;
		buf = realloc(buf, size);
	}
	free_items(&q->items, &q->n);
	free_items(&q->sizes, &q->nsizes);
	n = buf[0] << 8 | buf[1];
	for (i = 0; i < n; i++) {
		uint8_t *e = buf + 2 + 10 * i;
		const char *name = PTR(*(uint32_t *)(e + 2));
		int ys = e[6] << 8 | e[7], k, dup = 0;
		char nm[64];
		snprintf(nm, sizeof(nm), "%s", name);
		if (strlen(nm) > 5 && !strcasecmp(nm + strlen(nm) - 5, ".font"))
			nm[strlen(nm) - 5] = 0;
		for (k = 0; k < q->n; k++)
			if (!strcasecmp(q->items[k].name, nm))
				dup = 1;
		if (!dup)
			add_item(&q->items, &q->n, nm, 0, 0);
		if (!strcasecmp(nm, q->fontname)) {
			for (k = 0, dup = 0; k < q->nsizes; k++)
				if (q->sizes[k].kind == ys)
					dup = 1;
			if (!dup) {
				char s[16];
				snprintf(s, sizeof(s), "%d", ys);
				add_item(&q->sizes, &q->nsizes, s, ys, 0);
			}
		}
	}
	free(buf);
	if (q->n)
		qsort(q->items, q->n, sizeof(*q->items), icmp);
	q->sel = -1;
	for (i = 0; i < q->n; i++)
		if (!strcasecmp(q->items[i].name, q->fontname))
			q->sel = i;
}

static void scan_modes(struct asl *q, int minw, int maxw, int minh, int maxh)
{
	uint32_t id = 0xffffffff;
	int i;

	free_items(&q->items, &q->n);
	for (;;) {
		uint8_t name[56], dims[88];
		int w, h, md;
		id = gfx_next_mode(id);
		if (id == 0xffffffff)
			break;
		if (!gfx_mode_info(id, name, dims))
			continue;
		md = *(uint16_t *)(dims + 16);
		w = *(int16_t *)(dims + 30) + 1;
		h = *(int16_t *)(dims + 32) + 1;
		if (md < q->mindepth || w < minw || w > maxw || h < minh || h > maxh)
			continue;
		add_item(&q->items, &q->n, (char *)name + 16, md, id);
	}
	q->sel = -1;
	for (i = 0; i < q->n; i++)
		if (q->items[i].id == q->modeid)
			q->sel = i;
}

/* ---- drawing ---- */

static void field(struct awin *a, int x, int y, int w, const char *label, const char *s,
		  int active)
{
	int lx = x - (strlen(label) + 1) * cw();

	text(a, lx, y + 2, label, intui_pen(a, WBP_TEXTPEN), 0);
	gfx_rectfill(a->rp, a->bl + x, a->bt + y, a->bl + x + w - 1, a->bt + y + fh() + 3,
		     intui_pen(a, WBP_BACKGROUNDPEN));
	gfx_bevel(a->rp, a->bl + x, a->bt + y, w, fh() + 4, 1);
	text(a, x + 4, y + 2, s, intui_pen(a, WBP_TEXTPEN), (w - 8) / cw() - 1);
	if (active) {
		int n = strlen(s), max = (w - 8) / cw() - 1;
		int cx = x + 4 + (n < max ? n : max) * cw();
		gfx_complement(a->rp, a->bl + cx, a->bt + y + 2, a->bl + cx + cw() - 1,
			       a->bt + y + 1 + fh());
	}
}

static void list(struct awin *a, int x, int y, int w, struct item *v, int n, int top, int sel,
		 int rows, int is_files)
{
	int i;

	gfx_rectfill(a->rp, a->bl + x, a->bt + y - 2, a->bl + x + w - 1,
		     a->bt + y + rows * fh() + 1, intui_pen(a, WBP_BACKGROUNDPEN));
	gfx_bevel(a->rp, a->bl + x, a->bt + y - 2, w, rows * fh() + 4, 1);
	for (i = 0; i < rows && top + i < n; i++) {
		int k = top + i, pen = intui_pen(a, WBP_TEXTPEN);
		char line[80];
		if (k == sel) {
			gfx_rectfill(a->rp, a->bl + x + 2, a->bt + y + i * fh(),
				     a->bl + x + w - 3, a->bt + y + (i + 1) * fh() - 1,
				     intui_pen(a, WBP_FILLPEN));
			pen = intui_pen(a, WBP_FILLTEXTPEN);
		}
		snprintf(line, sizeof(line), "%s", v[k].name);
		text(a, x + 4, y + i * fh(), line, is_files && v[k].kind == 1 &&
		     pen != intui_pen(a, WBP_FILLTEXTPEN) ?
		     intui_pen(a, WBP_HIGHLIGHTTEXTPEN) : pen, (w - 8) / cw());
		if (v[k].kind == 1 && is_files)
			text(a, x + w - 6 * cw(), y + i * fh(), "Drawer", pen, 0);
	}
}

static void scroller(struct awin *a, int x, int y, int rows)
{
	int h = rows * fh() + 4;

	gfx_bevel(a->rp, a->bl + x, a->bt + y - 2, 16, h / 2, 0);
	gfx_bevel(a->rp, a->bl + x, a->bt + y - 2 + h / 2, 16, h - h / 2, 0);
	text(a, x + 4, y - 2 + h / 4 - fh() / 2, "^", intui_pen(a, WBP_TEXTPEN), 0);
	text(a, x + 4, y - 2 + 3 * h / 4 - fh() / 2, "v", intui_pen(a, WBP_TEXTPEN), 0);
}

static void draw(struct asl *q)
{
	struct awin *a = q->a;
	struct RastPort *rp = a->rp;
	int i, x = a->bl, y = a->bt;
	char buf[64];

	gfx_rectfill(rp, x, y, x + a->iw - 1, y + a->ih - 1, intui_pen(a, WBP_BACKGROUNDPEN));
	if (q->type == ASL_FontRequest) {
		list(a, 8, q->listy, LW - 72, q->items, q->n, q->top, q->sel, ROWS, 0);
		scroller(a, 8 + LW - 72 + 2, q->listy, ROWS);
		{
			int sel = -1;
			for (i = 0; i < q->nsizes; i++)
				if (q->sizes[i].kind == q->fontsize)
					sel = i;
			list(a, 8 + LW - 72 + 22, q->listy, 52, q->sizes, q->nsizes, 0, sel, ROWS, 0);
		}
		snprintf(buf, sizeof(buf), "%d", q->fontsize);
		field(a, 8 + 6 * cw(), q->fy, LW - 72 - 6 * cw(), "Font", q->fontname, 0);
		field(a, 8 + LW - 72 + 22, q->fy, 52, "", buf, 0);
	} else if (q->type == ASL_ScreenModeRequest) {
		list(a, 8, q->listy, LW, q->items, q->n, q->top, q->sel, ROWS, 0);
		scroller(a, 8 + LW + 2, q->listy, ROWS);
		snprintf(buf, sizeof(buf), "%d (%d colours)", q->depth, 1 << q->depth);
		field(a, 8 + 7 * cw(), q->fy, LW - 7 * cw() - 44, "Depth", buf, 0);
		gfx_bevel(rp, x + 8 + LW - 40, y + q->fy, 20, fh() + 4, 0);
		gfx_bevel(rp, x + 8 + LW - 18, y + q->fy, 20, fh() + 4, 0);
		text(a, 8 + LW - 40 + 6, q->fy + 2, "-", intui_pen(a, WBP_TEXTPEN), 0);
		text(a, 8 + LW - 18 + 6, q->fy + 2, "+", intui_pen(a, WBP_TEXTPEN), 0);
	} else {
		list(a, 8, q->listy, LW, q->items, q->n, q->top, q->sel, ROWS, 1);
		scroller(a, 8 + LW + 2, q->listy, ROWS);
		field(a, 8 + 7 * cw(), q->fy, LW - 7 * cw() + 18, "Drawer", q->drawer, q->field == 0);
		field(a, 8 + 7 * cw(), q->fy + fh() + 7, LW - 7 * cw() + 18, "File", q->file,
		      q->field == 1);
	}
	for (i = 0; i < q->nb; i++) {
		gfx_bevel(rp, x + q->bx[i], y + q->by, q->bw[i], q->bh, 0);
		text(a, q->bx[i] + (q->bw[i] - (int)strlen(q->lab[i]) * cw()) / 2, q->by + 3,
		     q->lab[i], intui_pen(a, WBP_TEXTPEN), 0);
	}
}

static void parent(struct asl *q)
{
	char *s = strrchr(q->drawer, '/');
	size_t l = strlen(q->drawer);

	if (s)
		*s = 0;
	else if (l && q->drawer[l - 1] != ':') {
		char *c = strchr(q->drawer, ':');
		if (c)
			c[1] = 0;
		else
			q->drawer[0] = 0;
	}
}

static int64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void scroll(struct asl *q, int y)
{
	if (y < q->listy + ROWS * fh() / 2)
		q->top -= ROWS - 1;
	else
		q->top += ROWS - 1;
	if (q->top > q->n - ROWS)
		q->top = q->n - ROWS;
	if (q->top < 0)
		q->top = 0;
}

static void type_into(char *s, size_t max, int c)
{
	size_t l = strlen(s);

	if (c == 8 && l)
		s[l - 1] = 0;
	else if (c >= 32 && c != 127 && l < max - 1) {
		s[l] = c;
		s[l + 1] = 0;
	}
}

static void click(struct asl *q, int x, int y)
{
	int listw = q->type == ASL_FontRequest ? LW - 72 : LW;
	int i;

	if (x >= 8 && x < 8 + listw && y >= q->listy && y < q->listy + ROWS * fh()) {
		int row = q->top + (y - q->listy) / fh();
		int64_t now = now_us(), dbl;
		if (row >= q->n)
			return;
		dbl = row == q->lastrow && now - q->lastclick < 500000;
		q->lastrow = row;
		q->lastclick = now;
		q->sel = row;
		if (q->type == ASL_FileRequest) {
			if (q->items[row].kind == 1) {
				size_t l = strlen(q->drawer);
				if (l && q->drawer[l - 1] != ':' && l < sizeof(q->drawer) - 2)
					strcat(q->drawer, "/");
				strncat(q->drawer, q->items[row].name,
					sizeof(q->drawer) - strlen(q->drawer) - 1);
				scan_files(q);
				q->lastrow = -1;
			} else {
				snprintf(q->file, sizeof(q->file), "%s", q->items[row].name);
				if (dbl)
					q->result = 1;
			}
		} else if (q->type == ASL_FontRequest) {
			snprintf(q->fontname, sizeof(q->fontname), "%s", q->items[row].name);
			scan_fonts(q);
			if (q->nsizes) {
				int best = 0;
				for (i = 0; i < q->nsizes; i++)
					if (abs(q->sizes[i].kind - q->fontsize) <
					    abs(q->sizes[best].kind - q->fontsize))
						best = i;
				q->fontsize = q->sizes[best].kind;
			}
			if (dbl)
				q->result = 1;
		} else {
			q->modeid = q->items[row].id;
			if (q->depth > q->items[row].kind)
				q->depth = q->items[row].kind;
			if (dbl)
				q->result = 1;
		}
		return;
	}
	if (x >= 8 + listw + 2 && x < 8 + listw + 18 && y >= q->listy - 2 &&
	    y < q->listy + ROWS * fh() + 2) {
		scroll(q, y);
		return;
	}
	if (q->type == ASL_FontRequest && x >= 8 + listw + 22 && x < 8 + listw + 74 &&
	    y >= q->listy && y < q->listy + ROWS * fh()) {
		int row = (y - q->listy) / fh();
		if (row < q->nsizes)
			q->fontsize = q->sizes[row].kind;
		return;
	}
	if (q->type == ASL_ScreenModeRequest && y >= q->fy && y < q->fy + fh() + 4) {
		int max = q->sel >= 0 ? q->items[q->sel].kind : q->maxdepth;
		if (max > q->maxdepth)
			max = q->maxdepth;
		if (x >= 8 + LW - 40 && x < 8 + LW - 20 && q->depth > q->mindepth)
			q->depth--;
		else if (x >= 8 + LW - 18 && x < 8 + LW + 2 && q->depth < max)
			q->depth++;
		return;
	}
	if (q->type == ASL_FileRequest && y >= q->fy && y < q->fy + 2 * fh() + 11 && x >= 8) {
		q->field = y >= q->fy + fh() + 7;
		return;
	}
	for (i = 0; i < q->nb; i++)
		if (x >= q->bx[i] && x < q->bx[i] + q->bw[i] && y >= q->by &&
		    y < q->by + q->bh) {
			if (i == 0)
				q->result = 1;
			else if (i == q->nb - 1)
				q->result = 0;
			else {                  /* Parent */
				parent(q);
				scan_files(q);
			}
		}
}

void asl_event(struct awin *a, const struct wbp_event *e)
{
	struct asl *q = a->aslreq;

	if (e->class == IDCMP_CLOSEWINDOW) {
		q->result = 0;
		return;
	}
	if (e->class == IDCMP_VANILLAKEY) {
		if (e->code == '\r' || e->code == '\n')
			q->result = 1;
		else if (e->code == 27)
			q->result = 0;
		else if (e->code == 9 && q->type == ASL_FileRequest)
			q->field = !q->field;
		else if (q->type == ASL_FileRequest) {
			if (q->field)
				type_into(q->file, sizeof(q->file), e->code);
			else {
				type_into(q->drawer, sizeof(q->drawer), e->code);
				scan_files(q);
			}
		} else if (q->type == ASL_FontRequest && e->code >= '0' && e->code <= '9')
			q->fontsize = q->fontsize % 100 * 10 + e->code - '0';
		else if (q->type == ASL_FontRequest && e->code == 8)
			q->fontsize /= 10;
		draw(q);
		return;
	}
	if (e->class != IDCMP_MOUSEBUTTONS || e->code != SELECTDOWN)
		return;
	click(q, e->mousex, e->mousey);
	draw(q);
}

/* ---- tags and the run ---- */

static uint32_t tagval(uint32_t tags, uint32_t tag, uint32_t def)
{
	extern struct TagItem *util_next_tag(struct TagItem **tp);
	struct TagItem *t = PTR(tags), *x;

	while ((x = util_next_tag(&t)))
		if (x->ti_Tag == tag)
			return x->ti_Data;
	return def;
}

/* the tag from the call, else from AllocAslRequest, else def */
static uint32_t tag2(uint32_t t1, uint32_t t2, uint32_t tag, uint32_t def)
{
	return tagval(t1, tag, tagval(t2, tag, def));
}

/* which screen: the parent window's, ASLxx_Screen, a public screen by name */
static uint32_t req_screen(uint32_t t1, uint32_t t2)
{
	uint32_t win = tag2(t1, t2, ASL_TB + 2, 0), scr = tag2(t1, t2, ASL_TB + 40, 0);
	uint32_t pub = tag2(t1, t2, ASL_TB + 41, 0);

	if (win) {
		struct awin *a = gui_by_window(win);
		if (a && a->as)
			return APTR_OF(a->as->scr);
		return 0;
	}
	if (scr && as_by_screen(scr))
		return scr;
	if (pub) {
		struct ascreen *as;
		for (as = gui_screens; as; as = as->next)
			if (as->pub && !strcmp(as->pubname, PTR(pub)))
				return APTR_OF(as->scr);
	}
	return 0;
}

static int run(uint8_t *r, uint32_t tags)
{
	struct asl q;
	struct awin *a;
	uint32_t t0 = *(uint32_t *)(r + REQ_TAGS);
	int type = r[REQ_TYPE];
	const char *deftitle = type == ASL_FontRequest ? "Select a font" :
			       type == ASL_ScreenModeRequest ? "Select a screen mode" :
			       "Select a file";
	const char *title = PTR(tag2(tags, t0, ASL_TB + 1, APTR_OF(deftitle)));
	const char *pos = PTR(tag2(tags, t0, ASL_TB + 18, APTR_OF(type ? "OK" : "OK")));
	const char *neg = PTR(tag2(tags, t0, ASL_TB + 19, APTR_OF("Cancel")));
	int iw = 8 + LW + 26, ih, i;

	memset(&q, 0, sizeof(q));
	q.type = type;
	q.lastrow = -1;
	q.listy = 6;
	q.fy = q.listy + ROWS * fh() + 8;
	if (type == ASL_FileRequest) {
		uint32_t f = tag2(tags, t0, ASL_TB + 8, *(uint32_t *)(r + 4));
		uint32_t d = tag2(tags, t0, ASL_TB + 9, *(uint32_t *)(r + 8));
		uint32_t p = tag2(tags, t0, ASL_TB + 10, 0), acc = tag2(tags, t0, ASL_TB + 62, 0);
		snprintf(q.drawer, sizeof(q.drawer), "%s", d ? (char *)PTR(d) : "");
		snprintf(q.file, sizeof(q.file), "%s", f ? (char *)PTR(f) : "");
		q.rejecticons = tag2(tags, t0, ASL_TB + 60, 0) != 0;
		q.drawersonly = tag2(tags, t0, ASL_TB + 47, 0) != 0;
		if (p && tag2(tags, t0, ASL_TB + 46, 0)) {
			snprintf(q.pattern, sizeof(q.pattern), "%s", (char *)PTR(p));
			pat_parse(q.pattern, q.tok, sizeof(q.tok), 1);
		}
		if (acc)
			snprintf(q.accept, sizeof(q.accept), "%s", (char *)PTR(acc));
		q.field = 1;
		q.by = q.fy + 2 * fh() + 16;
		q.nb = 3;
		q.lab[0] = pos;
		q.lab[1] = "Parent";
		q.lab[2] = neg;
	} else if (type == ASL_FontRequest) {
		uint32_t n = tag2(tags, t0, ASL_TB + 10, 0);
		struct TextAttr *ta = (struct TextAttr *)(r + 8);
		snprintf(q.fontname, sizeof(q.fontname), "%s",
			 n ? (char *)PTR(n) : ta->ta_Name ? (char *)PTR(ta->ta_Name) : "topaz");
		if (strlen(q.fontname) > 5 &&
		    !strcasecmp(q.fontname + strlen(q.fontname) - 5, ".font"))
			q.fontname[strlen(q.fontname) - 5] = 0;
		q.fontsize = tag2(tags, t0, ASL_TB + 11, ta->ta_YSize ? ta->ta_YSize : 8);
		q.by = q.fy + fh() + 14;
		q.nb = 2;
		q.lab[0] = pos;
		q.lab[1] = neg;
	} else {
		q.modeid = tag2(tags, t0, ASL_TB + 100, *(uint32_t *)r ? *(uint32_t *)r : 0x19000);
		q.depth = tag2(tags, t0, ASL_TB + 103, *(uint16_t *)(r + 12) ? *(uint16_t *)(r + 12) : 2);
		q.mindepth = tag2(tags, t0, ASL_TB + 120, 1);
		q.maxdepth = tag2(tags, t0, ASL_TB + 121, 8);
		if (q.depth > q.maxdepth)
			q.depth = q.maxdepth;
		if (q.depth < q.mindepth)
			q.depth = q.mindepth;
		q.by = q.fy + fh() + 14;
		q.nb = 2;
		q.lab[0] = pos;
		q.lab[1] = neg;
	}
	q.bh = fh() + 6;
	ih = q.by + q.bh + 6;
	for (i = 0; i < q.nb; i++) {
		q.bw[i] = 72;
		q.bx[i] = 8 + i * (iw - 16 - 72) / (q.nb - 1);
	}
	a = intui_open_plain(req_screen(tags, t0), title, iw, ih);
	if (!a)
		return 0;
	q.a = a;
	if (type == ASL_FileRequest)
		scan_files(&q);
	else if (type == ASL_FontRequest)
		scan_fonts(&q);
	else
		scan_modes(&q, tag2(tags, t0, ASL_TB + 116, 0), tag2(tags, t0, ASL_TB + 117, 16384),
			   tag2(tags, t0, ASL_TB + 118, 0), tag2(tags, t0, ASL_TB + 119, 16384));
	q.result = -1;
	a->aslreq = &q;
	draw(&q);
	while (q.result < 0 && gui_fd() >= 0) {
		gui_flush();
		gui_pump(-1);
	}
	a->aslreq = NULL;
	gui_close(a);
	gui_flush();
	if (q.result == 1) {
		if (type == ASL_FileRequest) {
			strcpy(PTR(*(uint32_t *)(r + 4)), q.file);
			strcpy(PTR(*(uint32_t *)(r + 8)), q.drawer);
			*(int32_t *)(r + 32) = 0;               /* fr_NumArgs */
			if (*(uint32_t *)(r + 52))              /* fr_Pattern */
				strcpy(PTR(*(uint32_t *)(r + 52)), q.pattern[0] ? q.pattern : "#?");
		} else if (type == ASL_FontRequest) {
			char *nm = PTR(*(uint32_t *)(r + REQ_NAME));
			struct TextAttr *ta = (struct TextAttr *)(r + 8);
			snprintf(nm, 64, "%s.font", q.fontname);
			ta->ta_Name = APTR_OF(nm);
			ta->ta_YSize = q.fontsize;
			ta->ta_Style = 0;
			ta->ta_Flags = 0x02;            /* FPF_DISKFONT */
			memcpy(r + 32, ta, 8);          /* fo_TAttr */
			*(uint32_t *)(r + 40) = 0;
		} else {
			int w, h;
			mode_size(q.modeid, &w, &h);
			*(uint32_t *)(r + 0) = q.modeid;
			*(uint32_t *)(r + 4) = w;
			*(uint32_t *)(r + 8) = h;
			*(uint16_t *)(r + 12) = q.depth;
			*(uint16_t *)(r + 14) = 0;      /* OSCAN_TEXT */
			*(uint16_t *)(r + 16) = 1;      /* autoscroll */
			*(uint32_t *)(r + 18) = w;
			*(uint32_t *)(r + 22) = h;
		}
	}
	free_items(&q.items, &q.n);
	free_items(&q.sizes, &q.nsizes);
	return q.result == 1;
}

static void asl_Alloc(struct frame *f)
{
	uint8_t *r = amem_alloc(REQ_SIZE);
	uint32_t tags = A(0);
	int type = D(0);

	if (type > ASL_ScreenModeRequest) {
		amem_free(r);
		RET(0);
		return;
	}
	r[REQ_TYPE] = type;
	*(uint32_t *)(r + REQ_TAGS) = tags;     /* read again at the request */
	*(uint32_t *)(r + REQ_NAME) = APTR_OF(amem_alloc(64));
	if (type == ASL_FileRequest) {
		*(uint32_t *)(r + 4) = APTR_OF(amem_alloc(256));
		*(uint32_t *)(r + 8) = APTR_OF(amem_alloc(256));
		*(uint32_t *)(r + 52) = APTR_OF(amem_alloc(256));        /* fr_Pattern */
		strcpy(PTR(*(uint32_t *)(r + 52)), "#?");
		if (tagval(tags, ASL_TB + 8, 0))
			snprintf(PTR(*(uint32_t *)(r + 4)), 256, "%s",
				 (char *)PTR(tagval(tags, ASL_TB + 8, 0)));
		if (tagval(tags, ASL_TB + 9, 0))
			snprintf(PTR(*(uint32_t *)(r + 8)), 256, "%s",
				 (char *)PTR(tagval(tags, ASL_TB + 9, 0)));
		*(uint32_t *)(r + 40) = tagval(tags, ASL_TB + 52, 0);  /* fr_UserData */
	} else if (type == ASL_FontRequest) {
		struct TextAttr *ta = (struct TextAttr *)(r + 8);
		char *nm = PTR(*(uint32_t *)(r + REQ_NAME));
		uint32_t n = tagval(tags, ASL_TB + 10, 0);
		snprintf(nm, 64, "%s", n ? (char *)PTR(n) : "topaz.font");
		ta->ta_Name = APTR_OF(nm);
		ta->ta_YSize = tagval(tags, ASL_TB + 11, 8);
		r[16] = tagval(tags, ASL_TB + 14, 1);   /* fo_FrontPen */
		r[17] = tagval(tags, ASL_TB + 15, 0);
		r[18] = tagval(tags, ASL_TB + 59, 1);   /* JAM2 */
		*(uint32_t *)(r + 20) = tagval(tags, ASL_TB + 52, 0);
	} else {
		*(uint32_t *)r = tagval(tags, ASL_TB + 100, 0x19000);
		*(uint16_t *)(r + 12) = tagval(tags, ASL_TB + 103, 2);
		*(uint32_t *)(r + 44) = tagval(tags, ASL_TB + 52, 0);  /* sm_UserData */
	}
	RET(r);
}

static void asl_AllocFile(struct frame *f)
{
	D(0) = 0;
	A(0) = 0;
	asl_Alloc(f);
}

static void asl_Free(struct frame *f) { (void)f; }

static void asl_Request(struct frame *f)
{
	RET(A(0) ? run(PTR(A(0)), A(1)) : 0);
}

static void asl_RequestFile(struct frame *f)
{
	RET(A(0) ? run(PTR(A(0)), 0) : 0);
}

static const struct fdent fd_asl[] = {
	{ 30, "AllocFileRequest", "" }, { 36, "FreeFileRequest", "a0" },
	{ 42, "RequestFile", "a0" }, { 48, "AllocAslRequest", "d0/a0" },
	{ 54, "FreeAslRequest", "a0" }, { 60, "AslRequest", "a0/a1" },
	{ 0, 0, 0 }
};

static const struct libimpl asl_impl[] = {
	{ "AllocFileRequest", asl_AllocFile }, { "FreeFileRequest", asl_Free },
	{ "RequestFile", asl_RequestFile }, { "AllocAslRequest", asl_Alloc },
	{ "FreeAslRequest", asl_Free }, { "AslRequest", asl_Request },
	{ NULL, NULL }
};

struct amilib lib_asl = {
	.name = "asl.library", .version = 40, .revision = 1,
	.fd = fd_asl, .impl = asl_impl, .possize = 8,
};
