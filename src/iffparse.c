/*
 * iffparse.library over DOS streams (InitIFFasDOS: iff_Stream is a
 * FileHandle), which is how Preferences editors read and write their
 * files.  Reading takes the whole stream into memory at the first
 * ParseIFF() and walks it; writing goes straight to the stream, sizes
 * patched by seeking back when PushChunk() was given IFFSIZE_UNKNOWN.
 * Supported: SCAN and STEP parsing, stop/prop/collection chunks,
 * StopOnExit, FindProp/FindCollection, CurrentChunk/ParentChunk, chunk
 * reads and writes.  Clipboard streams and custom stream hooks are not.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "af.h"
#include "dos_int.h"

#define IFFERR_EOF      (-1)
#define IFFERR_EOC      (-2)
#define IFFERR_NOSCOPE  (-3)
#define IFFERR_READ     (-5)
#define IFFERR_WRITE    (-6)
#define IFFERR_SEEK     (-7)
#define IFFERR_MANGLED  (-8)
#define IFFERR_NOTIFF   (-10)
#define IFFSIZE_UNKNOWN (-1)
#define IFFPARSE_SCAN   0
#define IFFPARSE_STEP   1
#define IFFPARSE_RAWSTEP 2

#define ID(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))
#define ID_FORM ID('F', 'O', 'R', 'M')
#define ID_LIST ID('L', 'I', 'S', 'T')
#define ID_CAT  ID('C', 'A', 'T', ' ')
#define ID_PROP ID('P', 'R', 'O', 'P')

/* the ContextNode programs see */
struct cnode {
	struct MinNode cn_Node;
	int32_t cn_ID, cn_Type, cn_Size, cn_Scan;
};

struct ctx {
	struct cnode *cn;               /* in Amiga memory */
	uint32_t start, end;            /* data, in the buffer / file */
	int container;
	int wsize_unknown;
	int32_t wpos;                   /* writing: offset of the size field */
};

struct sprop {                          /* StoredProperty + its key */
	uint32_t type, id;
	uint32_t *sp;                   /* { sp_Size, sp_Data } */
	struct sprop *next;
};

struct iff {
	uint32_t addr;                  /* the IFFHandle */
	int write;
	uint8_t *buf;
	uint32_t len, pos;
	int loaded;
	struct ctx stack[32];
	int depth;
	int pending_pop;                /* the last chunk returned: pop next */
	uint32_t stops[64][2], props[64][2], colls[64][2], exits[64][2];
	int nstops, nprops, ncolls, nexits;
	struct sprop *stored;
	struct iff *next;
};

static struct iff *iffs;

static struct iff *IFF(uint32_t addr)
{
	struct iff *i;

	for (i = iffs; i; i = i->next)
		if (i->addr == addr)
			return i;
	return NULL;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void set_depth(struct iff *f)
{
	((int32_t *)PTR(f->addr))[2] = f->depth;        /* iff_Depth */
}

static struct afh *stream(struct iff *f)
{
	return fh_priv(*(uint32_t *)PTR(f->addr));      /* iff_Stream */
}

static int load(struct iff *f)
{
	struct afh *a = stream(f);
	uint32_t cap = 65536;
	int32_t n;

	if (f->loaded)
		return 0;
	if (!a)
		return IFFERR_READ;
	f->buf = malloc(cap);
	f->len = 0;
	while ((n = fh_read(a, f->buf + f->len, cap - f->len)) > 0) {
		f->len += n;
		if (f->len == cap)
			f->buf = realloc(f->buf, cap *= 2);
	}
	f->loaded = 1;
	return 0;
}

static int in(uint32_t list[][2], int n, uint32_t type, uint32_t id)
{
	for (int i = 0; i < n; i++)
		if (list[i][0] == type && list[i][1] == id)
			return 1;
	return 0;
}

static void push(struct iff *f, uint32_t id, uint32_t type, uint32_t size,
		 uint32_t start, int container)
{
	struct ctx *c = &f->stack[f->depth++];

	c->cn = amem_alloc(sizeof(*c->cn));
	c->cn->cn_ID = id;
	c->cn->cn_Type = type;
	c->cn->cn_Size = size;
	c->start = start;
	c->end = start + size;
	c->container = container;
	set_depth(f);
}

static void pop(struct iff *f)
{
	struct ctx *c = &f->stack[--f->depth];

	f->pos = c->start + c->cn->cn_Size + (c->cn->cn_Size & 1);
	amem_free(c->cn);
	set_depth(f);
}

static void store_prop(struct iff *f, uint32_t type, uint32_t id,
		       const uint8_t *data, uint32_t size, int coll)
{
	struct sprop *p, **pp;
	uint8_t *d = amem_alloc(size + 1);

	memcpy(d, data, size);
	if (!coll)              /* a newer property replaces the older */
		for (pp = &f->stored; (p = *pp); pp = &p->next)
			if (p->type == type && p->id == id) {
				*pp = p->next;
				free(p);
				break;
			}
	p = calloc(1, sizeof(*p));
	p->type = type;
	p->id = id;
	/* CollectionItem { ci_Next, ci_Size, ci_Data } for collections */
	p->sp = amem_alloc(12);
	if (coll) {
		struct sprop *q;
		uint32_t next = 0;
		for (q = f->stored; q; q = q->next)
			if (q->type == type && q->id == id) {
				next = APTR_OF(q->sp);
				break;
			}
		p->sp[0] = next;
		p->sp[1] = size;
		p->sp[2] = APTR_OF(d);
		p->id |= 0;             /* collections are found by FindCollection */
	} else {
		p->sp[0] = size;
		p->sp[1] = APTR_OF(d);
	}
	p->next = f->stored;
	f->stored = p;
}

static int32_t parse(struct iff *f, int mode)
{
	int r = load(f);

	if (r)
		return r;
	for (;;) {
		struct ctx *top = f->depth ? &f->stack[f->depth - 1] : NULL;
		uint32_t id, size, type;

		if (f->pending_pop) {
			f->pending_pop = 0;
			pop(f);
			continue;
		}
		if (top && (f->pos >= top->end || !top->container)) {
			uint32_t t = top->cn->cn_Type, i = top->cn->cn_ID;
			int exit = top->container && in(f->exits, f->nexits, t, i);
			pop(f);
			if (exit || mode != IFFPARSE_SCAN)
				return IFFERR_EOC;
			continue;
		}
		if (!top && f->pos > 0)
			return IFFERR_EOF;
		if (f->pos + 8 > f->len)
			return top ? IFFERR_MANGLED : IFFERR_NOTIFF;
		id = be32(f->buf + f->pos);
		size = be32(f->buf + f->pos + 4);
		if (f->pos + 8 + size > f->len)
			return IFFERR_MANGLED;
		if (id == ID_FORM || id == ID_LIST || id == ID_CAT || id == ID_PROP) {
			type = f->pos + 12 <= f->len ? be32(f->buf + f->pos + 8) : 0;
			push(f, id, type, size - 4, f->pos + 12, 1);
			f->pos += 12;
			if (mode != IFFPARSE_SCAN)
				return 0;
			continue;
		}
		if (!top)
			return IFFERR_NOTIFF;
		type = top->cn->cn_Type;
		push(f, id, type, size, f->pos + 8, 0);
		f->pos += 8;
		if (in(f->props, f->nprops, type, id))
			store_prop(f, type, id, f->buf + f->pos, size, 0);
		if (in(f->colls, f->ncolls, type, id))
			store_prop(f, type, id | 0x80000000, f->buf + f->pos, size, 1);
		if (mode != IFFPARSE_SCAN || in(f->stops, f->nstops, type, id)) {
			f->pending_pop = 1;
			return 0;
		}
		pop(f);
	}
}

/* ---- writing ---- */

static int wr(struct iff *f, const void *p, uint32_t n)
{
	struct afh *a = stream(f);

	return a && fh_write(a, p, n) == (int32_t)n ? 0 : IFFERR_WRITE;
}

static int32_t push_w(struct iff *f, uint32_t type, uint32_t id, int32_t size)
{
	struct afh *a = stream(f);
	uint8_t h[12];
	int container = id == ID_FORM || id == ID_LIST || id == ID_CAT ||
			id == ID_PROP;
	off_t here;

	if (!a || f->depth == 32)
		return IFFERR_WRITE;
	fh_flush(a);
	here = lseek(a->wfd, 0, SEEK_CUR);
	h[0] = id >> 24, h[1] = id >> 16, h[2] = id >> 8, h[3] = id;
	{
		uint32_t s = size < 0 ? 0 : (uint32_t)size + (container ? 4 : 0);
		h[4] = s >> 24, h[5] = s >> 16, h[6] = s >> 8, h[7] = s;
	}
	h[8] = type >> 24, h[9] = type >> 16, h[10] = type >> 8, h[11] = type;
	if (wr(f, h, container ? 12 : 8))
		return IFFERR_WRITE;
	push(f, id, container ? type :
	     (f->depth ? (uint32_t)f->stack[f->depth - 1].cn->cn_Type : type),
	     0, 0, container);
	f->stack[f->depth - 1].wpos = here + 4;
	f->stack[f->depth - 1].wsize_unknown = size < 0;
	f->stack[f->depth - 1].cn->cn_Size = size < 0 ? 0 : size;
	return 0;
}

static int32_t pop_w(struct iff *f)
{
	struct afh *a = stream(f);
	struct ctx *c;
	uint32_t n;

	if (!f->depth || !a)
		return IFFERR_EOF;
	c = &f->stack[f->depth - 1];
	n = c->cn->cn_Scan + (c->container ? 4 : 0);
	if (c->cn->cn_Scan & 1)
		wr(f, "", 1);
	fh_flush(a);
	if (c->wsize_unknown || (uint32_t)c->cn->cn_Size != c->cn->cn_Scan) {
		off_t back = lseek(a->wfd, 0, SEEK_CUR);
		uint8_t s[4] = { n >> 24, n >> 16, n >> 8, n };
		lseek(a->wfd, c->wpos, SEEK_SET);
		if (write(a->wfd, s, 4) != 4)
			return IFFERR_WRITE;
		lseek(a->wfd, back, SEEK_SET);
	}
	n = c->cn->cn_Scan + (c->cn->cn_Scan & 1) + (c->container ? 12 : 8);
	amem_free(c->cn);
	f->depth--;
	set_depth(f);
	if (f->depth)
		f->stack[f->depth - 1].cn->cn_Scan += n -
			(f->stack[f->depth - 1].container ? 0 : 0);
	return 0;
}

/* ---- the vectors ---- */

static void ip_AllocIFF(struct frame *f)
{
	struct iff *i = calloc(1, sizeof(*i));

	i->addr = APTR_OF(amem_alloc(16));
	i->next = iffs;
	iffs = i;
	RET(i->addr);
}

static void ip_FreeIFF(struct frame *f)
{
	struct iff **pp, *i;

	for (pp = &iffs; (i = *pp); pp = &i->next)
		if (i->addr == A(0)) {
			*pp = i->next;
			free(i->buf);
			free(i);
			break;
		}
}

static void ip_Nop(struct frame *f) { (void)f; }

static void ip_OpenIFF(struct frame *f)
{
	struct iff *i = IFF(A(0));

	if (!i) {
		RET(IFFERR_NOSCOPE);
		return;
	}
	i->write = D(0) & 1;                    /* IFFF_WRITE */
	i->loaded = 0;
	i->pos = 0;
	i->depth = 0;
	i->pending_pop = 0;
	RET(0);
}

static void ip_CloseIFF(struct frame *f)
{
	struct iff *i = IFF(A(0));

	if (!i)
		return;
	while (i->write && i->depth)
		pop_w(i);
	while (!i->write && i->depth)
		pop(i);
	free(i->buf);
	i->buf = NULL;
	i->loaded = 0;
}

static void ip_ParseIFF(struct frame *f)
{
	struct iff *i = IFF(A(0));

	RET(i ? parse(i, D(0)) : IFFERR_NOSCOPE);
}

static void add_pairs(uint32_t list[][2], int *n, uint32_t *pairs, int count)
{
	for (int k = 0; k < count && *n < 64; k++) {
		list[*n][0] = pairs[2 * k];
		list[*n][1] = pairs[2 * k + 1];
		(*n)++;
	}
}

static void ip_StopChunk(struct frame *f)
{
	struct iff *i = IFF(A(0));
	uint32_t p[2] = { D(0), D(1) };

	if (i)
		add_pairs(i->stops, &i->nstops, p, 1);
	RET(0);
}

static void ip_StopChunks(struct frame *f)
{
	struct iff *i = IFF(A(0));

	if (i)
		add_pairs(i->stops, &i->nstops, PTR(A(1)), D(0));
	RET(0);
}

static void ip_PropChunk(struct frame *f)
{
	struct iff *i = IFF(A(0));
	uint32_t p[2] = { D(0), D(1) };

	if (i)
		add_pairs(i->props, &i->nprops, p, 1);
	RET(0);
}

static void ip_PropChunks(struct frame *f)
{
	struct iff *i = IFF(A(0));

	if (i)
		add_pairs(i->props, &i->nprops, PTR(A(1)), D(0));
	RET(0);
}

static void ip_CollectionChunk(struct frame *f)
{
	struct iff *i = IFF(A(0));
	uint32_t p[2] = { D(0), D(1) };

	if (i)
		add_pairs(i->colls, &i->ncolls, p, 1);
	RET(0);
}

static void ip_CollectionChunks(struct frame *f)
{
	struct iff *i = IFF(A(0));

	if (i)
		add_pairs(i->colls, &i->ncolls, PTR(A(1)), D(0));
	RET(0);
}

static void ip_StopOnExit(struct frame *f)
{
	struct iff *i = IFF(A(0));
	uint32_t p[2] = { D(0), D(1) };

	if (i)
		add_pairs(i->exits, &i->nexits, p, 1);
	RET(0);
}

static void ip_FindProp(struct frame *f)
{
	struct iff *i = IFF(A(0));
	struct sprop *p;

	RET(0);
	for (p = i ? i->stored : NULL; p; p = p->next)
		if (p->type == D(0) && p->id == D(1)) {
			RET(p->sp);
			return;
		}
}

static void ip_FindCollection(struct frame *f)
{
	struct iff *i = IFF(A(0));
	struct sprop *p;

	RET(0);
	for (p = i ? i->stored : NULL; p; p = p->next)
		if (p->type == D(0) && p->id == (D(1) | 0x80000000)) {
			RET(p->sp);
			return;
		}
}

static void ip_CurrentChunk(struct frame *f)
{
	struct iff *i = IFF(A(0));

	RET(i && i->depth ? APTR_OF(i->stack[i->depth - 1].cn) : 0);
}

static void ip_ParentChunk(struct frame *f)
{
	struct iff *i;
	int k;

	for (i = iffs; i; i = i->next)
		for (k = 1; k < i->depth; k++)
			if (APTR_OF(i->stack[k].cn) == A(0)) {
				RET(i->stack[k - 1].cn);
				return;
			}
	RET(0);
}

static void ip_FindPropContext(struct frame *f)
{
	struct iff *i = IFF(A(0));
	int k;

	RET(0);
	for (k = i ? i->depth - 1 : -1; k >= 0; k--)
		if (i->stack[k].container) {
			RET(i->stack[k].cn);
			return;
		}
}

static void ip_ReadChunkBytes(struct frame *f)
{
	struct iff *i = IFF(A(0));
	struct ctx *c;
	int32_t n = D(0), left;

	if (!i || !i->depth) {
		RET(IFFERR_EOF);
		return;
	}
	c = &i->stack[i->depth - 1];
	left = c->cn->cn_Size - c->cn->cn_Scan;
	if (n > left)
		n = left;
	if (n < 0)
		n = 0;
	memcpy(PTR(A(1)), i->buf + c->start + c->cn->cn_Scan, n);
	c->cn->cn_Scan += n;
	RET(n);
}

static void ip_ReadChunkRecords(struct frame *f)
{
	struct iff *i = IFF(A(0));
	struct ctx *c;
	int32_t rec = D(0), num = D(1), have;

	if (!i || !i->depth || rec <= 0) {
		RET(IFFERR_EOF);
		return;
	}
	c = &i->stack[i->depth - 1];
	have = (c->cn->cn_Size - c->cn->cn_Scan) / rec;
	if (num > have)
		num = have;
	memcpy(PTR(A(1)), i->buf + c->start + c->cn->cn_Scan, num * rec);
	c->cn->cn_Scan += num * rec;
	RET(num);
}

static void ip_WriteChunkBytes(struct frame *f)
{
	struct iff *i = IFF(A(0));

	if (!i || !i->depth || wr(i, PTR(A(1)), D(0))) {
		RET(IFFERR_WRITE);
		return;
	}
	i->stack[i->depth - 1].cn->cn_Scan += D(0);
	RET(D(0));
}

static void ip_WriteChunkRecords(struct frame *f)
{
	struct iff *i = IFF(A(0));
	uint32_t n = D(0) * D(1);

	if (!i || !i->depth || wr(i, PTR(A(1)), n)) {
		RET(IFFERR_WRITE);
		return;
	}
	i->stack[i->depth - 1].cn->cn_Scan += n;
	RET(D(1));
}

static void ip_PushChunk(struct frame *f)
{
	struct iff *i = IFF(A(0));

	RET(i ? push_w(i, D(0), D(1), (int32_t)D(2)) : IFFERR_NOSCOPE);
}

static void ip_PopChunk(struct frame *f)
{
	struct iff *i = IFF(A(0));

	RET(i ? pop_w(i) : IFFERR_NOSCOPE);
}

static void ip_OpenClipboard(struct frame *f) { RET(0); }
static void ip_Zero(struct frame *f) { RET(0); }

static int good(uint32_t id)
{
	for (int k = 0; k < 4; k++) {
		int c = (id >> (24 - 8 * k)) & 0xff;
		if (c < 0x20 || c > 0x7e)
			return 0;
	}
	return ((id >> 24) & 0xff) != ' ';
}

static void ip_GoodID(struct frame *f) { RET(good(D(0)) ? -1 : 0); }

static void ip_GoodType(struct frame *f)
{
	RET(good(D(0)) && D(0) != ID_FORM && D(0) != ID_LIST && D(0) != ID_CAT ?
	    -1 : 0);
}

static void ip_IDtoStr(struct frame *f)
{
	char *b = PTR(A(0));

	b[0] = D(0) >> 24, b[1] = D(0) >> 16, b[2] = D(0) >> 8, b[3] = D(0);
	b[4] = 0;
	RET(A(0));
}

static const struct libimpl iffparse_impl[] = {
	{ "AllocIFF", ip_AllocIFF }, { "FreeIFF", ip_FreeIFF },
	{ "InitIFFasDOS", ip_Nop }, { "InitIFF", ip_Nop },
	{ "InitIFFasClip", ip_Nop },
	{ "OpenIFF", ip_OpenIFF }, { "CloseIFF", ip_CloseIFF },
	{ "ParseIFF", ip_ParseIFF },
	{ "StopChunk", ip_StopChunk }, { "StopChunks", ip_StopChunks },
	{ "PropChunk", ip_PropChunk }, { "PropChunks", ip_PropChunks },
	{ "CollectionChunk", ip_CollectionChunk },
	{ "CollectionChunks", ip_CollectionChunks },
	{ "StopOnExit", ip_StopOnExit },
	{ "FindProp", ip_FindProp }, { "FindCollection", ip_FindCollection },
	{ "FindPropContext", ip_FindPropContext },
	{ "CurrentChunk", ip_CurrentChunk }, { "ParentChunk", ip_ParentChunk },
	{ "ReadChunkBytes", ip_ReadChunkBytes },
	{ "ReadChunkRecords", ip_ReadChunkRecords },
	{ "WriteChunkBytes", ip_WriteChunkBytes },
	{ "WriteChunkRecords", ip_WriteChunkRecords },
	{ "PushChunk", ip_PushChunk }, { "PopChunk", ip_PopChunk },
	{ "OpenClipboard", ip_OpenClipboard }, { "CloseClipboard", ip_Nop },
	{ "GoodID", ip_GoodID }, { "GoodType", ip_GoodType },
	{ "IDtoStr", ip_IDtoStr },
	{ "EntryHandler", ip_Zero }, { "ExitHandler", ip_Zero },
	{ NULL, NULL }
};

struct amilib lib_iffparse = {
	.name = "iffparse.library", .version = 40, .revision = 1,
	.fd = fd_iffparse, .impl = iffparse_impl, .possize = 8,
};
