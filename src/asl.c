/*
 * asl.library: a file requester drawn by amigafmt in a wbde window.
 *
 * A list of the drawer (drawers first, in the highlight pen), arrows to
 * scroll it, the drawer shown, the file name typed, and OK / Parent /
 * Cancel.  A click on a drawer enters it, on a file picks it (a double
 * click is OK); Return is OK, Esc Cancel.  Font and screen-mode
 * requesters answer Cancel.  Offsets are the 3.1 NDK asl_lib.fd's, checked
 * with tools/fdcheck.py (amitools has no asl_lib.fd); tags read: ASLFR_TitleText,
 * ASLFR_InitialFile, ASLFR_InitialDrawer (ASL_Hail/File/Dir).
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "gui.h"

#define ASL_TB 0x80080000
#define ROWS 10
#define LW 296                  /* list width */

struct asl {
	char drawer[256], file[128];
	char **names;
	int *isdir;
	int n, top, result;
	int64_t lastclick;
	int lastrow;
	int listy, fy, by, bh, bx[3], bw[3];
};

static int fh(void) { return gui_topaz->tf_YSize; }

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

static int cmp(const void *x, const void *y)
{
	return strcasecmp(*(char *const *)x, *(char *const *)y);
}

static void scan(struct asl *q)
{
	char *lp = path_to_linux(q->drawer, NULL), full[1200];
	DIR *d = lp ? opendir(lp) : NULL;
	struct dirent *e;
	char **dirs = NULL, **files = NULL;
	int nd = 0, nf = 0, i;

	for (i = 0; i < q->n; i++)
		free(q->names[i]);
	free(q->names);
	free(q->isdir);
	q->names = NULL;
	q->isdir = NULL;
	q->n = q->top = 0;
	while (d && (e = readdir(d))) {
		struct stat st;
		if (e->d_name[0] == '.')
			continue;
		snprintf(full, sizeof(full), "%s/%s", lp, e->d_name);
		if (stat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			dirs = realloc(dirs, (nd + 1) * sizeof(char *));
			dirs[nd++] = strdup(e->d_name);
		} else {
			files = realloc(files, (nf + 1) * sizeof(char *));
			files[nf++] = strdup(e->d_name);
		}
	}
	if (d)
		closedir(d);
	free(lp);
	if (nd)
		qsort(dirs, nd, sizeof(char *), cmp);
	if (nf)
		qsort(files, nf, sizeof(char *), cmp);
	q->n = nd + nf;
	q->names = malloc((q->n + 1) * sizeof(char *));
	q->isdir = malloc((q->n + 1) * sizeof(int));
	for (i = 0; i < nd; i++)
		q->names[i] = dirs[i], q->isdir[i] = 1;
	for (i = 0; i < nf; i++)
		q->names[nd + i] = files[i], q->isdir[nd + i] = 0;
	free(dirs);
	free(files);
}

static void draw(struct awin *a, struct asl *q)
{
	struct RastPort *rp = a->rp;
	int i, x = a->bl, y = a->bt;
	static const char *lab[3] = { "OK", "Parent", "Cancel" };

	gfx_rectfill(rp, x, y, x + a->iw - 1, y + a->ih - 1,
		     gui_pens[WBP_BACKGROUNDPEN]);
	gfx_bevel(rp, x + 8, y + q->listy - 2, LW, ROWS * fh() + 4, 1);
	for (i = 0; i < ROWS && q->top + i < q->n; i++) {
		int k = q->top + i;
		char line[64];
		snprintf(line, sizeof(line), "%s%s", q->names[k],
			 q->isdir[k] ? " (dir)" : "");
		if (!q->isdir[k] && !strcmp(q->names[k], q->file))
			gfx_rectfill(rp, x + 10, y + q->listy + i * fh(), x + 6 + LW - 1,
				     y + q->listy + (i + 1) * fh() - 1,
				     gui_pens[WBP_FILLPEN]);
		text(a, 12, q->listy + i * fh(), line,
		     q->isdir[k] ? gui_pens[WBP_HIGHLIGHTTEXTPEN] :
		     gui_pens[WBP_TEXTPEN], (LW - 8) / gui_topaz->tf_XSize);
	}
	/* scroll arrows: the upper and lower half of a column */
	gfx_bevel(rp, x + 8 + LW + 2, y + q->listy - 2, 16, ROWS * fh() / 2 + 2, 0);
	gfx_bevel(rp, x + 8 + LW + 2, y + q->listy + ROWS * fh() / 2, 16,
		  ROWS * fh() / 2 + 2, 0);
	text(a, 8 + LW + 6, q->listy + ROWS * fh() / 4 - fh() / 2, "^",
	     gui_pens[WBP_TEXTPEN], 0);
	text(a, 8 + LW + 6, q->listy + ROWS * fh() * 3 / 4 - fh() / 2, "v",
	     gui_pens[WBP_TEXTPEN], 0);
	text(a, 8, q->fy, "Drawer", gui_pens[WBP_TEXTPEN], 0);
	gfx_bevel(rp, x + 64, y + q->fy - 2, LW - 38, fh() + 4, 1);
	text(a, 68, q->fy, q->drawer, gui_pens[WBP_TEXTPEN],
	     (LW - 46) / gui_topaz->tf_XSize);
	text(a, 8, q->fy + fh() + 6, "File", gui_pens[WBP_TEXTPEN], 0);
	gfx_bevel(rp, x + 64, y + q->fy + fh() + 4, LW - 38, fh() + 4, 1);
	text(a, 68, q->fy + fh() + 6, q->file, gui_pens[WBP_TEXTPEN],
	     (LW - 46) / gui_topaz->tf_XSize - 1);
	/* the cursor after the name */
	{
		int cx = x + 68 + strlen(q->file) * gui_topaz->tf_XSize;
		gfx_complement(rp, cx, y + q->fy + fh() + 6,
			       cx + gui_topaz->tf_XSize - 1, y + q->fy + 2 * fh() + 5);
	}
	for (i = 0; i < 3; i++) {
		gfx_bevel(rp, x + q->bx[i], y + q->by, q->bw[i], q->bh, 0);
		text(a, q->bx[i] + (q->bw[i] - strlen(lab[i]) *
				    gui_topaz->tf_XSize) / 2,
		     q->by + 3, lab[i], gui_pens[WBP_TEXTPEN], 0);
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

void asl_event(struct awin *a, const struct wbp_event *e)
{
	struct asl *q = a->aslreq;
	int x = e->mousex, y = e->mousey, i;

	if (e->class == IDCMP_CLOSEWINDOW) {
		q->result = 0;
		return;
	}
	if (e->class == IDCMP_VANILLAKEY) {
		size_t l = strlen(q->file);
		if (e->code == '\r')
			q->result = 1;
		else if (e->code == 27)
			q->result = 0;
		else if (e->code == 8 && l)
			q->file[l - 1] = 0;
		else if (e->code >= 32 && e->code != 127 && l < sizeof(q->file) - 1) {
			q->file[l] = e->code;
			q->file[l + 1] = 0;
		}
		draw(a, q);
		return;
	}
	if (e->class != IDCMP_MOUSEBUTTONS || e->code != SELECTDOWN)
		return;
	if (x >= 8 && x < 8 + LW && y >= q->listy && y < q->listy + ROWS * fh()) {
		int row = q->top + (y - q->listy) / fh();
		struct timeval tv;
		int64_t now;
		gettimeofday(&tv, NULL);
		now = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
		if (row >= q->n)
			return;
		if (q->isdir[row]) {
			size_t l = strlen(q->drawer);
			if (l && q->drawer[l - 1] != ':' && l < sizeof(q->drawer) - 2)
				strcat(q->drawer, "/");
			strncat(q->drawer, q->names[row],
				sizeof(q->drawer) - strlen(q->drawer) - 1);
			scan(q);
		} else {
			snprintf(q->file, sizeof(q->file), "%s", q->names[row]);
			if (row == q->lastrow && now - q->lastclick < 500000)
				q->result = 1;
			q->lastrow = row;
			q->lastclick = now;
		}
		draw(a, q);
		return;
	}
	if (x >= 8 + LW + 2 && x < 8 + LW + 18 && y >= q->listy - 2 &&
	    y < q->listy + ROWS * fh() + 2) {
		if (y < q->listy + ROWS * fh() / 2)
			q->top -= ROWS - 1;
		else
			q->top += ROWS - 1;
		if (q->top > q->n - ROWS)
			q->top = q->n - ROWS;
		if (q->top < 0)
			q->top = 0;
		draw(a, q);
		return;
	}
	for (i = 0; i < 3; i++)
		if (x >= q->bx[i] && x < q->bx[i] + q->bw[i] && y >= q->by &&
		    y < q->by + q->bh) {
			if (i == 0)
				q->result = 1;
			else if (i == 2)
				q->result = 0;
			else {
				parent(q);
				scan(q);
				draw(a, q);
			}
		}
}

static uint32_t tagval(uint32_t tags, uint32_t tag, uint32_t def)
{
	extern struct TagItem *util_next_tag(struct TagItem **tp);
	struct TagItem *t = PTR(tags), *x;

	while ((x = util_next_tag(&t)))
		if (x->ti_Tag == tag)
			return x->ti_Data;
	return def;
}

/* FileRequester: fr_File at +4, fr_Drawer at +8; type kept at +250 */
static int run(uint8_t *fr, uint32_t tags)
{
	struct asl q;
	struct awin *a;
	const char *title = PTR(tagval(tags, ASL_TB + 1,
				       *(uint32_t *)(fr + 244) ?
				       *(uint32_t *)(fr + 244) :
				       APTR_OF("Select a file")));
	uint32_t f = tagval(tags, ASL_TB + 8, *(uint32_t *)(fr + 4));
	uint32_t d = tagval(tags, ASL_TB + 9, *(uint32_t *)(fr + 8));
	int iw = 8 + LW + 26, ih, i;

	if (fr[250] != 0) {             /* font or screen mode requester */
		if (af_trace)
			af_warn("asl: only file requesters are provided");
		return 0;
	}
	memset(&q, 0, sizeof(q));
	q.lastrow = -1;
	snprintf(q.drawer, sizeof(q.drawer), "%s", d ? (char *)PTR(d) : "");
	snprintf(q.file, sizeof(q.file), "%s", f ? (char *)PTR(f) : "");
	q.listy = 6;
	q.fy = q.listy + ROWS * fh() + 8;
	q.by = q.fy + 2 * fh() + 14;
	q.bh = fh() + 6;
	ih = q.by + q.bh + 6;
	for (i = 0; i < 3; i++) {
		q.bw[i] = 72;
		q.bx[i] = 8 + i * (iw - 16 - 72) / 2;
	}
	a = intui_open_plain(title, iw, ih);
	if (!a)
		return 0;
	scan(&q);
	q.result = -1;
	a->aslreq = &q;
	draw(a, &q);
	while (q.result < 0 && gui_fd() >= 0) {
		gui_flush();
		gui_pump(-1);
	}
	a->aslreq = NULL;
	gui_close(a);
	gui_flush();
	if (q.result == 1) {
		strcpy(PTR(*(uint32_t *)(fr + 4)), q.file);
		strcpy(PTR(*(uint32_t *)(fr + 8)), q.drawer);
		*(int32_t *)(fr + 32) = 0;      /* fr_NumArgs */
	}
	for (i = 0; i < q.n; i++)
		free(q.names[i]);
	free(q.names);
	free(q.isdir);
	return q.result == 1;
}

static void asl_Alloc(struct frame *f)
{
	uint8_t *r = amem_alloc(256);
	uint32_t tags = A(0);

	*(uint32_t *)(r + 4) = APTR_OF(amem_alloc(256));
	*(uint32_t *)(r + 8) = APTR_OF(amem_alloc(256));
	r[250] = D(0);                          /* ASL_FileRequest == 0 */
	*(uint32_t *)(r + 244) = tagval(tags, ASL_TB + 1, 0);
	if (tagval(tags, ASL_TB + 8, 0))
		snprintf(PTR(*(uint32_t *)(r + 4)), 256, "%s",
			 (char *)PTR(tagval(tags, ASL_TB + 8, 0)));
	if (tagval(tags, ASL_TB + 9, 0))
		snprintf(PTR(*(uint32_t *)(r + 8)), 256, "%s",
			 (char *)PTR(tagval(tags, ASL_TB + 9, 0)));
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
