/*
 * diskfont.library: bitmap fonts from disk.
 *
 * A font is a directory of sizes plus a contents file: FONTS:name.font is
 * a FontContentsHeader (FCH_ID $0f00, or TFCH_ID $0f02 with tags at the
 * end of each name) listing "name/size" files, and each of those is a
 * hunk file whose first hunk is MOVEQ #-1,D0; RTS followed by a
 * DiskFontHeader with the TextFont inside (diskfont/diskfont.h).
 * OpenDiskFont LoadSegs the file and adds the TextFont to GfxBase's
 * TextFonts list, so graphics' OpenFont finds it next time.
 *
 * A name with a path in it (V36: "PROGDIR:fonts/own.font") is taken
 * as it is and the size files are found next to it.  A size that is not
 * on disk gets the nearest one that is: fonts are not scaled.  Colour
 * fonts load (the TextFont is the head of a ColorTextFont) but draw in
 * one colour.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "gui.h"
#include "dos_int.h"

#define FCH_ID   0x0f00
#define TFCH_ID  0x0f02
#define DFH_ID   0x0f80
#define FPF_ROMFONT  0x01
#define FPF_DISKFONT 0x02
#define FPF_DESIGNED 0x40
#define AFF_MEMORY 0x0001
#define AFF_DISK   0x0002
#define AFF_TAGGED 0x10000

extern void exec_addtail(void *l, void *n);

/* GfxBase->TextFonts (graphics/gfxbase.h: after 140 bytes) */
struct List *gfx_fontlist(void)
{
	return (struct List *)((uint8_t *)GfxBase + 140);
}

static const char *base_name(const char *n)
{
	const char *p = strrchr(n, '/'), *q = strrchr(n, ':');

	if (q && (!p || q > p))
		p = q;
	return p ? p + 1 : n;
}

/* a font already in memory (the list, or topaz) with this name and size */
struct TextFont *diskfont_find(const char *name, int ysize, int exact)
{
	struct List *l = GfxBase ? gfx_fontlist() : NULL;
	struct Node *n;
	struct TextFont *best = NULL;
	const char *bn = base_name(name);

	if (l && l->lh_Head)
		for (n = PTR(l->lh_Head); n && n->ln_Succ; n = PTR(n->ln_Succ)) {
			struct TextFont *tf = (struct TextFont *)n;
			if (!n->ln_Name || strcasecmp(PTR(n->ln_Name), bn))
				continue;
			if (tf->tf_YSize == ysize)
				return tf;
			if (!exact && (!best || abs(tf->tf_YSize - ysize) <
				       abs(best->tf_YSize - ysize)))
				best = tf;
		}
	return best;
}

static void add_font(struct TextFont *tf)
{
	struct List *l = gfx_fontlist();

	if (!l->lh_Head) {                      /* NewList, first use */
		l->lh_Head = APTR_OF(&l->lh_Tail);
		l->lh_Tail = 0;
		l->lh_TailPred = APTR_OF(&l->lh_Head);
	}
	exec_addtail(l, &tf->tf_Message.mn_Node);
}

/* the contents file: Linux path of the .font for an Amiga font name */
static char *contents_path(const char *name)
{
	char buf[512];

	if (strchr(name, ':') || strchr(name, '/'))
		snprintf(buf, sizeof(buf), "%s", name);
	else
		snprintf(buf, sizeof(buf), "FONTS:%s", name);
	return path_to_linux(buf, NULL);
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(n > 0 ? n : 1);
	if (b && fread(b, 1, n, f) != (size_t)n) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*len = n;
	return b;
}

static uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* entry i of a contents file: its file name and size */
static int fc_entry(const uint8_t *fc, size_t len, int i, char *file, int *ysize)
{
	const uint8_t *e = fc + 4 + i * 260;

	if (4 + (size_t)(i + 1) * 260 > len)
		return 0;
	memcpy(file, e, 255);
	file[255] = 0;
	if (be16(fc) == TFCH_ID) {
		int tags = be16(e + 254);
		/* the tags take the end of the name field */
		if (tags * 8 < 254)
			file[254 - tags * 8] = 0;
	}
	*ysize = be16(e + 256);
	return 1;
}

struct TextFont *diskfont_open(const char *name, int ysize)
{
	struct TextFont *tf = diskfont_find(name, ysize, 1);
	char *cp, file[256], dir[512], apath[600], *lp;
	uint8_t *fc, *dfh;
	size_t len;
	int i, n, best = -1, bestsize = 0, err;
	BPTR seg;

	if (tf) {
		tf->tf_Accessors++;
		return tf;
	}
	cp = contents_path(name);
	fc = cp ? slurp(cp, &len) : NULL;
	if (!fc || len < 4 || (be16(fc) != FCH_ID && be16(fc) != TFCH_ID)) {
		if (af_trace)
			af_warn("OpenDiskFont(%s/%d): no contents file %s", name,
				ysize, cp ? cp : "?");
		free(cp);
		free(fc);
		/* the nearest in memory then, as diskfont does without the disk */
		tf = diskfont_find(name, ysize, 0);
		if (tf)
			tf->tf_Accessors++;
		return tf;
	}
	n = be16(fc + 2);
	for (i = 0; i < n; i++) {
		int ys;
		if (!fc_entry(fc, len, i, file, &ys))
			break;
		if (best < 0 || abs(ys - ysize) < abs(bestsize - ysize)) {
			best = i;
			bestsize = ys;
		}
	}
	if (best < 0) {
		free(cp);
		free(fc);
		return NULL;
	}
	/* already loaded at the nearest size? */
	tf = diskfont_find(name, bestsize, 1);
	if (tf) {
		free(cp);
		free(fc);
		tf->tf_Accessors++;
		return tf;
	}
	fc_entry(fc, len, best, file, &bestsize);
	free(fc);
	/* the size files are beside the contents file */
	snprintf(dir, sizeof(dir), "%s", cp);
	free(cp);
	if (strrchr(dir, '/'))
		*strrchr(dir, '/') = 0;
	snprintf(apath, sizeof(apath), "%s/%s", dir, file);
	lp = path_to_linux(apath, "/");
	seg = lp ? hunk_load(lp, &err) : 0;
	free(lp);
	if (!seg) {
		if (af_trace)
			af_warn("OpenDiskFont(%s/%d): cannot load %s", name, ysize, apath);
		return NULL;
	}
	dfh = (uint8_t *)BADDR(seg) + 8;        /* after MOVEQ #-1,D0; RTS */
	if (be16(dfh + 14) != DFH_ID) {
		hunk_unload(seg);
		return NULL;
	}
	*(uint32_t *)(dfh + 18) = seg;          /* dfh_Segment */
	tf = (struct TextFont *)(dfh + 54);     /* dfh_TF */
	if (!tf->tf_Message.mn_Node.ln_Name) {
		/* the name is the font's, not the size file's */
		char *nm = amem_alloc(strlen(base_name(name)) + 1);
		strcpy(nm, base_name(name));
		tf->tf_Message.mn_Node.ln_Name = APTR_OF(nm);
	} else {
		/* dfh_Name holds "name.font"? keep what the file says only if
		   it is the font's name; it usually is */
		char *nm = PTR(tf->tf_Message.mn_Node.ln_Name);
		if (strcasecmp(nm, base_name(name))) {
			char *c = amem_alloc(strlen(base_name(name)) + 1);
			strcpy(c, base_name(name));
			tf->tf_Message.mn_Node.ln_Name = APTR_OF(c);
		}
	}
	tf->tf_Message.mn_Node.ln_Type = 12;    /* NT_FONT */
	tf->tf_Flags = (tf->tf_Flags & ~FPF_ROMFONT) | FPF_DISKFONT;
	tf->tf_Accessors = 1;
	add_font(tf);
	if (af_trace)
		af_warn("OpenDiskFont(%s/%d) = %s %d (%dx%d)", name, ysize, apath,
			tf->tf_YSize, tf->tf_XSize, tf->tf_YSize);
	return tf;
}

static void df_OpenDiskFont(struct frame *f)
{
	struct TextAttr *ta = PTR(A(0));

	if (!ta || !ta->ta_Name) {
		RET(0);
		return;
	}
	if (!strcasecmp(base_name(PTR(ta->ta_Name)), "topaz.font")) {
		RET(gfx_font_for(ta));
		return;
	}
	RET(diskfont_open(PTR(ta->ta_Name), ta->ta_YSize));
}

/* ---- AvailFonts ---- */

uint32_t gfx_availfonts(uint8_t *buf, int32_t size, uint32_t flags);

struct afent {
	char name[64];
	int ysize, style, flags, type;
};

static int add_ent(struct afent **v, int *n, int *cap, const char *name,
		   int ysize, int style, int flags, int type)
{
	int i;

	for (i = 0; i < *n; i++)
		if ((*v)[i].ysize == ysize && (*v)[i].type == type &&
		    !strcasecmp((*v)[i].name, name))
			return 0;
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 32;
		*v = realloc(*v, *cap * sizeof(**v));
	}
	snprintf((*v)[*n].name, 64, "%s", name);
	(*v)[*n].ysize = ysize;
	(*v)[*n].style = style;
	(*v)[*n].flags = flags;
	(*v)[*n].type = type;
	(*n)++;
	return 1;
}

static void df_AvailFonts(struct frame *f)
{
	RET(gfx_availfonts(PTR(A(0)), D(0), D(1)));
}

/* AvailFonts: 0, or how many more bytes the buffer needs */
uint32_t gfx_availfonts(uint8_t *buf, int32_t size, uint32_t flags)
{
	int tagged = (flags & AFF_TAGGED) != 0, esize = tagged ? 14 : 10;
	struct afent *v = NULL;
	int n = 0, cap = 0, i;
	size_t need, names = 0;

	if (flags & AFF_MEMORY) {
		struct List *l = GfxBase ? gfx_fontlist() : NULL;
		if (gui_topaz)
			add_ent(&v, &n, &cap, "topaz.font", gui_topaz->tf_YSize,
				0, FPF_ROMFONT | FPF_DESIGNED, AFF_MEMORY);
		if (l && l->lh_Head) {
			struct Node *nd;
			for (nd = PTR(l->lh_Head); nd->ln_Succ; nd = PTR(nd->ln_Succ)) {
				struct TextFont *tf = (struct TextFont *)nd;
				char nm[64];
				if (!nd->ln_Name)
					continue;
				snprintf(nm, sizeof(nm), "%s", (char *)PTR(nd->ln_Name));
				if (!strstr(nm, ".font"))
					strncat(nm, ".font", sizeof(nm) - strlen(nm) - 1);
				add_ent(&v, &n, &cap, nm, tf->tf_YSize, tf->tf_Style,
					tf->tf_Flags, AFF_MEMORY);
			}
		}
	}
	if (flags & AFF_DISK) {
		char *dir = path_to_linux("FONTS:", NULL);
		DIR *d = dir ? opendir(dir) : NULL;
		struct dirent *de;

		while (d && (de = readdir(d))) {
			size_t l = strlen(de->d_name), len;
			char p[1024], file[256];
			uint8_t *fc;
			int k, ys;

			if (l < 6 || strcasecmp(de->d_name + l - 5, ".font"))
				continue;
			snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
			fc = slurp(p, &len);
			if (!fc || len < 4 || (be16(fc) != FCH_ID && be16(fc) != TFCH_ID)) {
				free(fc);
				continue;
			}
			for (k = 0; k < be16(fc + 2) && fc_entry(fc, len, k, file, &ys); k++) {
				const uint8_t *e = fc + 4 + k * 260;
				add_ent(&v, &n, &cap, de->d_name, ys, e[258],
					e[259] | FPF_DISKFONT, AFF_DISK);
			}
			free(fc);
		}
		if (d)
			closedir(d);
		free(dir);
	}
	for (i = 0; i < n; i++)
		names += strlen(v[i].name) + 1;
	need = 2 + (size_t)n * esize + names;
	if ((int32_t)need > size) {
		free(v);
		return need - size;
	}
	{
		uint8_t *e = buf + 2;
		char *s = (char *)buf + 2 + n * esize;
		buf[0] = n >> 8;
		buf[1] = n;
		for (i = 0; i < n; i++, e += esize) {
			strcpy(s, v[i].name);
			e[0] = v[i].type >> 8;
			e[1] = v[i].type | (v[i].type == AFF_DISK ? 8 : 0);	/* AFF_BITMAP */
			*(uint32_t *)(e + 2) = APTR_OF(s);
			e[6] = v[i].ysize >> 8;
			e[7] = v[i].ysize;
			e[8] = v[i].style;
			e[9] = v[i].flags;
			if (tagged)
				*(uint32_t *)(e + 10) = 0;      /* tta_Tags */
			s += strlen(s) + 1;
		}
	}
	free(v);
	return 0;
}

/* ---- contents ---- */

static void df_NewFontContents(struct frame *f)
{
	const char *name = PTR(A(1));
	char p[1024];
	uint8_t *fc, *out;
	size_t len;

	RET(0);
	if (!name)
		return;
	if (A(0))       /* the fonts directory, by lock */
		snprintf(p, sizeof(p), "%s/%s", lock_path(A(0)), name);
	else {
		char *cp = contents_path(name);
		if (!cp)
			return;
		snprintf(p, sizeof(p), "%s", cp);
		free(cp);
	}
	fc = slurp(p, &len);
	if (!fc || len < 4) {
		free(fc);
		return;
	}
	out = amem_alloc(len);
	memcpy(out, fc, len);
	free(fc);
	RET(out);
}

static void df_DisposeFontContents(struct frame *f)
{
	if (A(1))
		amem_free(PTR(A(1)));
}

static void df_Zero(struct frame *f) { RET(0); }

static const struct fdent fd_diskfont[] = {
	{ 30, "OpenDiskFont", "a0" },
	{ 36, "AvailFonts", "a0/d0/d1" },
	{ 42, "NewFontContents", "a0/a1" },
	{ 48, "DisposeFontContents", "a1" },
	{ 54, "NewScaledDiskFont", "a0/a1" },
	{ 0, NULL, NULL }
};

static const struct libimpl diskfont_impl[] = {
	{ "OpenDiskFont", df_OpenDiskFont },
	{ "AvailFonts", df_AvailFonts },
	{ "NewFontContents", df_NewFontContents },
	{ "DisposeFontContents", df_DisposeFontContents },
	{ "NewScaledDiskFont", df_Zero },
	{ NULL, NULL }
};

struct amilib lib_diskfont = {
	.name = "diskfont.library", .version = 40, .revision = 1,
	.fd = fd_diskfont, .impl = diskfont_impl,
};
