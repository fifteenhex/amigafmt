/*
 * The connection to wbde ($WBDE_SOCKET, default /tmp/wbde.sock), made on
 * first use: the Workbench Screen, topaz, DrawInfo, windows as shared
 * memory buffers of pens, damage reports, and events turned into
 * IntuiMessages by intuition.c.  Single threaded: exec's Wait(), WaitPort()
 * and GetMsg() pump the socket.  See wbde's doc/client-protocol.md.
 */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "gui.h"

struct awin *gui_wins;
struct wbp_screen gui_screen;
struct Screen *wb_screen;
struct TextFont *gui_topaz;
struct TextAttr *gui_topaz_attr;
struct DrawInfo *gui_dri;
uint16_t gui_pens[16];

static int fd = -1, failed;
static uint32_t serial;
static uint8_t *rbuf;
static size_t rlen, rcap;

/* events that arrived while waiting for a reply: dispatched later */
struct qev {
	uint32_t win;
	struct wbp_event e;
	int resize;
	struct wbp_window m;
	int mfd;
};
static struct qev queue[256];
static int nqueue;

int gui_fd(void)
{
	return fd;
}

void gui_send(uint32_t type, uint32_t win, const void *p, uint32_t len)
{
	struct wbp_hdr h = { type, len, win, ++serial };
	struct iovec iov[2] = { { &h, sizeof(h) }, { (void *)p, len } };
	struct msghdr m;

	if (fd < 0)
		return;
	memset(&m, 0, sizeof(m));
	m.msg_iov = iov;
	m.msg_iovlen = len ? 2 : 1;
	if (sendmsg(fd, &m, MSG_NOSIGNAL) < 0) {
		af_warn("lost the connection to wbde");
		close(fd);
		fd = -1;
	}
}

/* one whole message into rbuf (blocking); 0 on success */
static int recv_msg(struct wbp_hdr *h, uint8_t **payload, int *rfd)
{
	*rfd = -1;
	for (;;) {
		if (rlen >= sizeof(*h)) {
			memcpy(h, rbuf, sizeof(*h));
			if (rlen >= sizeof(*h) + h->len) {
				*payload = rbuf + sizeof(*h);
				return 0;
			}
		}
		if (rcap - rlen < 65536) {
			rcap = rcap ? rcap * 2 : 131072;
			rbuf = realloc(rbuf, rcap);
		}
		{
			char cbuf[CMSG_SPACE(sizeof(int))];
			struct iovec iov = { rbuf + rlen, rcap - rlen };
			struct msghdr m;
			struct cmsghdr *cm;
			ssize_t n;

			memset(&m, 0, sizeof(m));
			m.msg_iov = &iov;
			m.msg_iovlen = 1;
			m.msg_control = cbuf;
			m.msg_controllen = sizeof(cbuf);
			n = recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				return -1;
			for (cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
				if (cm->cmsg_level == SOL_SOCKET &&
				    cm->cmsg_type == SCM_RIGHTS)
					memcpy(rfd, CMSG_DATA(cm), sizeof(int));
			rlen += n;
		}
	}
}

static void consume(const struct wbp_hdr *h)
{
	size_t n = sizeof(*h) + h->len;

	memmove(rbuf, rbuf + n, rlen - n);
	rlen -= n;
}

static void map_window(struct awin *a, const struct wbp_window *m, int mfd)
{
	if (af_trace)
		fprintf(stderr, "amigafmt: window %u buffer %dx%d stride %d fd %d\n",
			a->id, m->width, m->height, m->stride, mfd);
	if (a->pix)
		munmap(a->pix, a->size);
	a->iw = m->width;
	a->ih = m->height;
	a->stride = m->stride;
	a->size = m->size;
	a->bl = m->bl;
	a->bt = m->bt;
	a->br = m->br;
	a->bb = m->bb;
	a->pix = mmap(NULL, a->size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	if (a->pix == MAP_FAILED)
		a->pix = NULL;
	close(mfd);
	a->w->LeftEdge = m->left;
	a->w->TopEdge = m->top;
	a->w->Width = m->width + m->bl + m->br;
	a->w->Height = m->height + m->bt + m->bb;
	a->w->BorderLeft = m->bl;
	a->w->BorderTop = m->bt;
	a->w->BorderRight = m->br;
	a->w->BorderBottom = m->bb;
	a->w->GZZWidth = m->width;
	a->w->GZZHeight = m->height;
}

static struct awin *by_id(uint32_t id)
{
	struct awin *a;

	for (a = gui_wins; a; a = a->next)
		if (a->id == id)
			return a;
	return NULL;
}

/* anything that is not the awaited reply */
static void other(const struct wbp_hdr *h, const uint8_t *p, int mfd)
{
	struct qev *q;

	if (h->type == WBP_EVENT || h->type == WBP_WINDOW) {
		if (nqueue == 256) {
			if (mfd >= 0)
				close(mfd);
			return;
		}
		q = &queue[nqueue++];
		memset(q, 0, sizeof(*q));
		q->win = h->win;
		q->mfd = -1;
		if (h->type == WBP_EVENT)
			memcpy(&q->e, p, sizeof(q->e));
		else {
			q->resize = 1;
			memcpy(&q->m, p, sizeof(q->m));
			q->mfd = mfd;
		}
		return;
	}
	if (h->type == WBP_SCREEN)
		memcpy(&gui_screen, p, sizeof(gui_screen));
	if (mfd >= 0)
		close(mfd);
}

/* send a request, wait for the reply with its serial */
int gui_call(uint32_t type, uint32_t win, const void *p, uint32_t len,
		uint32_t reply, void *out, size_t outlen, int *outfd,
		uint32_t *outwin)
{
	struct wbp_hdr h;
	uint8_t *pl;
	int mfd;

	gui_send(type, win, p, len);
	while (fd >= 0) {
		if (recv_msg(&h, &pl, &mfd)) {
			af_warn("lost the connection to wbde");
			close(fd);
			fd = -1;
			return -1;
		}
		if (h.serial == serial && (h.type == reply || h.type == WBP_ERROR)) {
			int ret = h.type == WBP_ERROR ? *(int32_t *)pl : 0;
			if (!ret && out)
				memcpy(out, pl, h.len < outlen ? h.len : outlen);
			if (outwin)
				*outwin = h.win;
			if (outfd)
				*outfd = mfd;
			else if (mfd >= 0)
				close(mfd);
			consume(&h);
			return ret;
		}
		other(&h, pl, mfd);
		consume(&h);
	}
	return -1;
}

static void dispatch_queue(void)
{
	while (nqueue) {
		struct qev q = queue[0];
		struct awin *a;

		memmove(queue, queue + 1, sizeof(queue[0]) * --nqueue);
		a = by_id(q.win);
		if (q.resize) {
			if (a && q.mfd >= 0) {
				map_window(a, &q.m, q.mfd);
				intui_resized(a);
			} else if (q.mfd >= 0)
				close(q.mfd);
		} else if (a)
			intui_event(a, &q.e);
	}
}

/* ---- the Workbench screen ---- */

static void build_font(void)
{
	struct wbp_font wf;
	struct wbp_glyph *g;
	struct wbp_hdr h;
	uint8_t *pl, *data;
	uint32_t *loc;
	int16_t *space, *kern;
	int mfd, i, n;
	struct TextFont *tf;
	char *name;

	gui_send(WBP_GET_FONT, 0, NULL, 0);
	for (;;) {
		if (recv_msg(&h, &pl, &mfd))
			return;
		if (h.type == WBP_FONT && h.serial == serial)
			break;
		other(&h, pl, mfd);
		consume(&h);
	}
	memcpy(&wf, pl, sizeof(wf));
	g = (struct wbp_glyph *)(pl + sizeof(wf));
	n = wf.hichar - wf.lochar + 2;          /* + the "no glyph" one */
	tf = amem_alloc(sizeof(*tf));
	data = amem_alloc((size_t)wf.modulo * wf.ysize);
	memcpy(data, pl + sizeof(wf) + 257 * sizeof(*g),
	       (size_t)wf.modulo * wf.ysize);
	loc = amem_alloc(n * 4);
	space = amem_alloc(n * 2);
	kern = amem_alloc(n * 2);
	for (i = 0; i < n; i++) {
		struct wbp_glyph *x = i == n - 1 ? &g[256] : &g[wf.lochar + i];
		loc[i] = (uint32_t)x->offset << 16 | x->width;
		space[i] = x->space;
		kern[i] = x->kern;
	}
	name = amem_alloc(40);
	snprintf(name, 40, "%s", wf.name);
	tf->tf_Message.mn_Node.ln_Type = 12;    /* NT_FONT */
	tf->tf_Message.mn_Node.ln_Name = APTR_OF(name);
	tf->tf_YSize = wf.ysize;
	tf->tf_XSize = wf.xsize;
	tf->tf_Baseline = wf.baseline;
	tf->tf_Flags = 0x02 | 0x40;             /* FPF_ROMFONT | FPF_DESIGNED */
	tf->tf_BoldSmear = 1;
	tf->tf_Accessors = 1;
	tf->tf_LoChar = wf.lochar;
	tf->tf_HiChar = wf.hichar;
	tf->tf_CharData = APTR_OF(data);
	tf->tf_Modulo = wf.modulo;
	tf->tf_CharLoc = APTR_OF(loc);
	tf->tf_CharSpace = APTR_OF(space);
	tf->tf_CharKern = APTR_OF(kern);
	consume(&h);
	gui_topaz = tf;
	gui_topaz_attr = amem_alloc(sizeof(*gui_topaz_attr));
	gui_topaz_attr->ta_Name = APTR_OF(name);
	gui_topaz_attr->ta_YSize = wf.ysize;
	gui_topaz_attr->ta_Flags = 0x02 | 0x40;
}

static void build_screen(void)
{
	struct Screen *s = amem_alloc(sizeof(*s));
	struct wbp_screen *g = &gui_screen;
	uint16_t *pens = amem_alloc(32);
	uint8_t *cm = amem_alloc(64);
	int i, top = g->wbortop - g->font_ysize - 1;

	s->Width = g->width;
	s->Height = g->height;
	s->Flags = 0x0001 | 0x0100;             /* WBENCHSCREEN | PUBLICSCREEN */
	s->Title = s->DefaultTitle = APTR_OF("Workbench Screen");
	s->BarHeight = g->barheight - 1;
	s->BarVBorder = 1;
	s->BarHBorder = 5;
	s->MenuVBorder = 2;
	s->MenuHBorder = 4;
	s->WBorTop = top > 0 ? top : 2;
	s->WBorLeft = 4;
	s->WBorRight = 4;
	s->WBorBottom = 2;
	s->Font = APTR_OF(gui_topaz_attr);
	s->ViewPort.DWidth = g->width;
	s->ViewPort.DHeight = g->height;
	s->ViewPort.Modes = 0x8000;             /* HIRES */
	s->ViewPort.ColorMap = APTR_OF(cm);
	*(uint16_t *)(cm + 2) = 256;            /* cm->Count */
	s->BitMap.BytesPerRow = g->width / 8;
	s->BitMap.Rows = g->height;
	s->BitMap.Depth = g->depth;
	gfx_init_rp(&s->RastPort);
	s->RastPort.BitMap = APTR_OF(&s->BitMap);
	s->DetailPen = g->pens[WBP_DETAILPEN];
	s->BlockPen = g->pens[WBP_BLOCKPEN];
	wb_screen = s;

	for (i = 0; i < WBP_NUMPENS; i++)
		gui_pens[i] = pens[i] = g->pens[i];
	pens[WBP_NUMPENS] = 0xffff;
	gui_dri = amem_alloc(sizeof(*gui_dri));
	gui_dri->dri_Version = 2;
	gui_dri->dri_NumPens = WBP_NUMPENS;
	gui_dri->dri_Pens = APTR_OF(pens);
	gui_dri->dri_Font = APTR_OF(gui_topaz);
	gui_dri->dri_Depth = g->depth;
	gui_dri->dri_ResX = 22;
	gui_dri->dri_ResY = 44;
	gui_dri->dri_Flags = 1;                 /* DRIF_NEWLOOK */
}

int gui_connect(void)
{
	struct sockaddr_un a;
	struct wbp_hello hello;
	const char *path = getenv("WBDE_SOCKET");

	if (fd >= 0)
		return 0;
	if (failed)
		return -1;
	if (!path)
		path = WBP_SOCKET_DEFAULT;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		if (af_trace)
			af_warn("no wbde at %s", path);
		if (fd >= 0)
			close(fd);
		fd = -1;
		failed = 1;
		return -1;
	}
	memset(&hello, 0, sizeof(hello));
	hello.version = WBP_VERSION;
	snprintf(hello.name, sizeof(hello.name), "%s", af_progname);
	if (gui_call(WBP_HELLO, 0, &hello, sizeof(hello), WBP_SCREEN, &gui_screen,
		 sizeof(gui_screen), NULL, NULL)) {
		failed = 1;
		return -1;
	}
	build_font();
	if (!gui_topaz) {
		failed = 1;
		return -1;
	}
	build_screen();
	{
		extern struct Library *IntuitionBase;
		if (IntuitionBase) {    /* ActiveScreen, FirstScreen */
			*(uint32_t *)((uint8_t *)IntuitionBase + 56) = APTR_OF(wb_screen);
			*(uint32_t *)((uint8_t *)IntuitionBase + 60) = APTR_OF(wb_screen);
		}
	}
	if (GfxBase)    /* GfxBase->DefaultFont, offset 154 */
		*(uint32_t *)((uint8_t *)GfxBase + 154) = APTR_OF(gui_topaz);
	return 0;
}

/* ---- windows ---- */

struct awin *gui_open(const struct wbp_open *o)
{
	struct wbp_window m;
	struct awin *a;
	uint32_t id;
	int mfd = -1;

	if (gui_connect())
		return NULL;
	if (gui_call(WBP_OPEN, 0, o, sizeof(*o), WBP_WINDOW, &m, sizeof(m), &mfd,
		 &id) || mfd < 0)
		return NULL;
	a = calloc(1, sizeof(*a));
	a->id = id;
	a->w = amem_alloc(sizeof(struct Window));
	map_window(a, &m, mfd);
	a->next = gui_wins;
	gui_wins = a;
	return a;
}

void gui_close(struct awin *a)
{
	struct awin **pp;

	gui_send(WBP_CLOSE, a->id, NULL, 0);
	for (pp = &gui_wins; *pp; pp = &(*pp)->next)
		if (*pp == a) {
			*pp = a->next;
			break;
		}
	if (a->pix)
		munmap(a->pix, a->size);
	free(a);
}

struct awin *gui_by_window(APTR w)
{
	struct awin *a;

	for (a = gui_wins; a; a = a->next)
		if (APTR_OF(a->w) == w)
			return a;
	return NULL;
}

struct awin *gui_by_rp(APTR rp)
{
	struct awin *a;

	for (a = gui_wins; a; a = a->next)
		if (APTR_OF(a->rp) == rp)
			return a;
	return NULL;
}

void gui_damage(struct awin *a, int x0, int y0, int x1, int y1)
{
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 >= a->iw)
		x1 = a->iw - 1;
	if (y1 >= a->ih)
		y1 = a->ih - 1;
	if (x0 > x1 || y0 > y1)
		return;
	if (!a->dirty) {
		a->dirty = 1;
		a->dx0 = x0, a->dy0 = y0, a->dx1 = x1, a->dy1 = y1;
		return;
	}
	if (x0 < a->dx0) a->dx0 = x0;
	if (y0 < a->dy0) a->dy0 = y0;
	if (x1 > a->dx1) a->dx1 = x1;
	if (y1 > a->dy1) a->dy1 = y1;
}

void *gui_map(int mfd, size_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);

	close(mfd);
	return p == MAP_FAILED ? NULL : p;
}

void gui_unmap(void *p, size_t size)
{
	if (p)
		munmap(p, size);
}

void gui_flush(void)
{
	struct awin *a;

	as_flush();
	for (a = gui_wins; a; a = a->next)
		if (a->dirty) {
			struct wbp_rect r = { a->dx0, a->dy0, a->dx1 - a->dx0 + 1,
					      a->dy1 - a->dy0 + 1 };
			a->dirty = 0;
			if (af_trace)
				fprintf(stderr, "amigafmt: damage win %u %d,%d %dx%d\n",
					a->id, r.x, r.y, r.w, r.h);
			gui_send(WBP_DAMAGE, a->id, &r, sizeof(r));
		}
}

/* read what has arrived (waiting up to timeout_ms first); 1 if anything */
int gui_pump(int timeout_ms)
{
	struct pollfd p;
	int any = nqueue > 0;

	dispatch_queue();
	if (fd < 0)
		return any;
	p.fd = fd;
	p.events = POLLIN;
	if (rlen < sizeof(struct wbp_hdr) && poll(&p, 1, timeout_ms) <= 0)
		return any;
	do {
		struct wbp_hdr h;
		uint8_t *pl;
		int mfd;
		if (recv_msg(&h, &pl, &mfd)) {
			af_warn("wbde went away");
			close(fd);
			fd = -1;
			break;
		}
		other(&h, pl, mfd);
		consume(&h);
		p.revents = 0;
	} while (rlen >= sizeof(struct wbp_hdr) ||
		 (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)));
	dispatch_queue();
	return 1;
}

int gui_obtain_pen(uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t rgb[3] = { r, g, b };
	uint32_t pen = 1;

	if (gui_connect() || gui_call(WBP_OBTAIN_PEN, 0, rgb, 3, WBP_PEN, &pen,
				  sizeof(pen), NULL, NULL))
		return 1;
	return pen;
}
