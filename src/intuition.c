/*
 * intuition.library, as a CLI program sees it without a display.
 *
 * Requesters are asked on the terminal; screens and windows cannot be
 * opened (the calls fail with NULL, which programs check), and anything
 * else stops the program with the usual "unimplemented" message.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "af.h"

struct sbuf {
	char s[1024];
	int n;
};

static void put_sbuf(int c, void *ctx)
{
	struct sbuf *b = ctx;
	if (b->n < (int)sizeof(b->s) - 1)
		b->s[b->n++] = c;
	b->s[b->n] = 0;
}

/* ask on the terminal; gadgets "A|B|C" give 1, 2, ... and 0 for the last */
static int ask(const char *title, const char *body, const char *gadgets)
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

static void i_EasyRequestArgs(struct frame *f)
{
	uint32_t *es = PTR(A(1));
	struct sbuf body = { "", 0 }, gad = { "", 0 };
	uint32_t data = A(3);

	if (!es) {
		RET(0);
		return;
	}
	if (es[3])
		data = fmt_do(PTR(es[3]), data, put_sbuf, &body, 0);
	if (es[4])
		fmt_do(PTR(es[4]), data, put_sbuf, &gad, 0);
	RET(ask(es[2] ? PTR(es[2]) : NULL, body.s, gad.s));
}

/* IntuiText chains: IText at +12, NextText at +16 */
static void itext(uint32_t it, char *buf, size_t len)
{
	size_t n = 0;

	buf[0] = 0;
	for (; it && n < len - 2; it = *(uint32_t *)((char *)PTR(it) + 16)) {
		uint32_t t = *(uint32_t *)((char *)PTR(it) + 12);
		n += snprintf(buf + n, len - n, "%s%s", n ? "\n" : "",
			      t ? (char *)PTR(t) : "");
	}
}

static void i_AutoRequest(struct frame *f)
{
	char body[1024], pos[128], neg[128], g[260];

	itext(A(1), body, sizeof(body));
	itext(A(2), pos, sizeof(pos));
	itext(A(3), neg, sizeof(neg));
	if (*pos)
		snprintf(g, sizeof(g), "%s|%s", pos, neg);
	else
		snprintf(g, sizeof(g), "%s", neg);
	RET(*pos ? ask(NULL, body, g) == 1 : 0);
}

static void i_DisplayBeep(struct frame *f)
{
	if (isatty(2) && write(2, "\a", 1) < 0) {
		/* no beep then */
	}
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

static void i_Zero(struct frame *f)
{
	RET(0);
}

static void i_NoDisplay(struct frame *f)
{
	if (af_trace)
		af_warn("no display: a screen or window was refused");
	RET(0);
}

static void i_Nop(struct frame *f)
{
	(void)f;
}

static const struct libimpl intuition_impl[] = {
	{ "EasyRequestArgs", i_EasyRequestArgs },
	{ "AutoRequest", i_AutoRequest },
	{ "BuildEasyRequestArgs", i_Zero }, { "FreeSysRequest", i_Nop },
	{ "DisplayBeep", i_DisplayBeep }, { "CurrentTime", i_CurrentTime },
	{ "DoubleClick", i_Zero },
	{ "OpenScreen", i_NoDisplay }, { "OpenScreenTagList", i_NoDisplay },
	{ "OpenWindow", i_NoDisplay }, { "OpenWindowTagList", i_NoDisplay },
	{ "LockPubScreen", i_NoDisplay }, { "UnlockPubScreen", i_Nop },
	{ "OpenWorkBench", i_Zero }, { "WBenchToFront", i_Zero },
	{ "CloseWindow", i_Nop }, { "CloseScreen", i_Zero },
	{ "GetScreenData", i_Zero }, { "ViewAddress", i_Zero },
	{ "LockIBase", i_Zero }, { "UnlockIBase", i_Nop },
	{ NULL, NULL }
};

struct amilib lib_intuition = {
	.name = "intuition.library", .version = 40, .revision = 85,
	.fd = fd_intuition, .impl = intuition_impl,
	.possize = 0x200,               /* IntuitionBase, all NULL/zero */
};

/*
 * graphics.library and gadtools.library: the bases only, so programs
 * that open them to read GfxBase or for an optional feature start (lha
 * opens both); any call stops the program with its name.  amitools has
 * no gadtools_lib.fd, so its vectors have no names here and an
 * unimplemented call is reported by offset alone.
 */
struct amilib lib_gadtools = {
	.name = "gadtools.library", .version = 40, .revision = 4,
	.minslots = 32, .possize = 8,
};

struct amilib lib_graphics = {
	.name = "graphics.library", .version = 40, .revision = 42,
	.fd = fd_graphics, .impl = NULL,
	.possize = 0x300,               /* GfxBase, all zero */
};
