/*
 * AmigaOS hunk executables (what LoadSeg() loads).
 *
 * Each hunk is allocated as LoadSeg does it: a longword with the
 * allocation size, the BPTR to the next hunk, then the contents.  The
 * seglist is the BPTR to the first "next" longword, so programs that
 * walk their own seglist find what they expect.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "af.h"

#define HUNK_NAME          0x3e8
#define HUNK_CODE          0x3e9
#define HUNK_DATA          0x3ea
#define HUNK_BSS           0x3eb
#define HUNK_RELOC32       0x3ec
#define HUNK_RELOC16       0x3ed
#define HUNK_RELOC8        0x3ee
#define HUNK_EXT           0x3ef
#define HUNK_SYMBOL        0x3f0
#define HUNK_DEBUG         0x3f1
#define HUNK_END           0x3f2
#define HUNK_HEADER        0x3f3
#define HUNK_OVERLAY       0x3f5
#define HUNK_BREAK         0x3f6
#define HUNK_DREL32        0x3f7        /* V37 LoadSeg: = RELOC32SHORT */
#define HUNK_RELOC32SHORT  0x3fc
#define HUNK_RELRELOC32    0x3fd

struct rd {
	const uint8_t *p, *end;
	int bad;
};

static uint32_t get32(struct rd *r)
{
	uint32_t v;

	if (r->end - r->p < 4) {
		r->bad = 1;
		return 0;
	}
	v = (uint32_t)r->p[0] << 24 | r->p[1] << 16 | r->p[2] << 8 | r->p[3];
	r->p += 4;
	return v;
}

static uint16_t get16(struct rd *r)
{
	uint16_t v;

	if (r->end - r->p < 2) {
		r->bad = 1;
		return 0;
	}
	v = r->p[0] << 8 | r->p[1];
	r->p += 2;
	return v;
}

static int skip(struct rd *r, uint32_t longs)
{
	if ((uint32_t)(r->end - r->p) / 4 < longs) {
		r->bad = 1;
		return 0;
	}
	r->p += longs * 4;
	return 1;
}

static uint8_t *readfile(const char *path, size_t *len)
{
	FILE *fp = fopen(path, "rb");
	uint8_t *buf = NULL;
	size_t n = 0, cap = 0, got;

	if (!fp)
		return NULL;
	do {
		if (n == cap) {
			cap = cap ? cap * 2 : 65536;
			buf = realloc(buf, cap);
		}
		got = fread(buf + n, 1, cap - n, fp);
		n += got;
	} while (got);
	fclose(fp);
	*len = n;
	return buf;
}

void hunk_unload(BPTR seg)
{
	while (seg) {
		uint32_t *h = BADDR(seg);
		seg = h[0];
		amem_free(h - 1);
	}
}

/* returns the seglist, or 0 with *err set to an AmigaDOS error */
BPTR hunk_load(const char *path, int *err)
{
	size_t len;
	uint8_t *file = readfile(path, &len);
	struct rd r;
	uint32_t first = 0, last = 0, n, i, cur, type;
	uint32_t **hunks = NULL, *sizes = NULL;
	BPTR seg = 0;

	*err = 0;
	if (!file) {
		*err = dos_errno(errno);
		return 0;
	}
	r.p = file;
	r.end = file + len;
	r.bad = 0;
	if (get32(&r) != HUNK_HEADER) {
		*err = ERROR_FILE_NOT_OBJECT;
		goto out;
	}
	while ((n = get32(&r)) && !r.bad)       /* resident library names */
		skip(&r, n);
	get32(&r);                              /* table size */
	first = get32(&r);
	last = get32(&r);
	if (r.bad || last < first || last - first > 10000)
		goto bad;
	n = last - first + 1;
	hunks = calloc(n, sizeof(*hunks));
	sizes = calloc(n, sizeof(*sizes));
	for (i = 0; i < n; i++) {
		uint32_t s = get32(&r);
		if ((s & 0xc0000000) == 0xc0000000)
			get32(&r);              /* extended memory attributes */
		sizes[i] = (s & 0x3fffffff) * 4;
		hunks[i] = amem_alloc(sizes[i] + 8);
		if (!hunks[i]) {
			*err = ERROR_NO_FREE_STORE;
			goto out;
		}
		hunks[i][0] = sizes[i] + 8;
		if (i)
			hunks[i - 1][1] = MKBADDR(&hunks[i][1]);
	}
	if (r.bad)
		goto bad;

	cur = 0;
	while (r.p < r.end && !r.bad) {
		type = get32(&r) & 0x3fffffff;
		switch (type) {
		case HUNK_NAME:
		case HUNK_DEBUG:
			skip(&r, get32(&r));
			break;
		case HUNK_CODE:
		case HUNK_DATA:
			n = get32(&r);
			if (cur >= last - first + 1 || n * 4 > sizes[cur] ||
			    (uint32_t)(r.end - r.p) / 4 < n)
				goto bad;
			memcpy(&hunks[cur][2], r.p, n * 4);
			r.p += n * 4;
			break;
		case HUNK_BSS:
			get32(&r);
			if (cur >= last - first + 1)
				goto bad;
			break;
		case HUNK_RELOC32:
		case HUNK_RELOC32SHORT:
		case HUNK_DREL32:
		case HUNK_RELRELOC32: {
			int shortf = type != HUNK_RELOC32;
			const uint8_t *start = r.p;
			for (;;) {
				uint32_t cnt = shortf ? get16(&r) : get32(&r);
				uint32_t tgt, off, base, mine, v;
				uint8_t *q;
				if (!cnt || r.bad)
					break;
				tgt = shortf ? get16(&r) : get32(&r);
				if (tgt < first || tgt > last || cur > last - first)
					goto bad;
				tgt -= first;
				base = (uint32_t)(uintptr_t)&hunks[tgt][2];
				mine = (uint32_t)(uintptr_t)&hunks[cur][2];
				while (cnt--) {
					off = shortf ? get16(&r) : get32(&r);
					if (r.bad || off + 4 > sizes[cur])
						goto bad;
					q = (uint8_t *)&hunks[cur][2] + off;
					v = (uint32_t)q[0] << 24 | q[1] << 16 |
					    q[2] << 8 | q[3];
					v += base;
					if (type == HUNK_RELRELOC32)
						v -= mine + off;
					q[0] = v >> 24, q[1] = v >> 16;
					q[2] = v >> 8, q[3] = v;
				}
			}
			if (shortf && ((r.p - start) & 2))
				get16(&r);              /* pad to a longword */
			break;
		}
		case HUNK_SYMBOL:
			while ((n = get32(&r)) && !r.bad)
				skip(&r, n + 1);
			break;
		case HUNK_END:
			cur++;
			break;
		case HUNK_OVERLAY:
		case HUNK_BREAK:
			af_warn("%s: overlay executables are not supported", path);
			*err = ERROR_BAD_HUNK;
			goto out;
		default:
			af_warn("%s: unsupported hunk type $%x", path, type);
			*err = ERROR_BAD_HUNK;
			goto out;
		}
	}
	if (r.bad)
		goto bad;
	for (i = 0; i <= last - first; i++)
		cache_flush(hunks[i], sizes[i] + 8);
	seg = MKBADDR(&hunks[0][1]);
	hunks = NULL;
	goto out;
bad:
	af_warn("%s: truncated or corrupt hunk file", path);
	*err = ERROR_BAD_HUNK;
out:
	if (hunks) {
		for (i = 0; i <= last - first; i++)
			if (hunks[i])
				amem_free(hunks[i]);
	}
	free(hunks);
	free(sizes);
	free(file);
	return seg;
}

/* which hunk of seg holds addr: for crash reports */
int hunk_where(uint32_t addr, BPTR seg, int *hunk, uint32_t *off)
{
	int i = 0;

	while (seg) {
		uint32_t *h = BADDR(seg);
		uint32_t start = (uint32_t)(uintptr_t)(h + 1);
		uint32_t size = h[-1] - 8;
		if (addr >= start && addr < start + size) {
			*hunk = i;
			*off = addr - start;
			return 1;
		}
		seg = h[0];
		i++;
	}
	return 0;
}
