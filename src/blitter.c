/*
 * The blitter, for programs that drive it (after OwnBlitter(), as the OS
 * allows): main.c's store emulation hands writes to $DFF040..$DFF074
 * here, and a write to BLTSIZE (or BLTSIZH, ECS) runs the whole blit at
 * once, so the blitter is never busy.  "Chip RAM" is any memory here, so
 * the pointers are used as they are.
 *
 * Area mode with the barrel shifters, first/last word masks, modulos,
 * descending mode and area fill (inclusive and exclusive), and line mode
 * as the Hardware Reference Manual describes it.
 */
#include <string.h>
#include "af.h"

static uint16_t regs[0x80];             /* $DFF000.. as words, the blitter's part used */
#define R(off) regs[(off) >> 1]

static uint32_t ptr(int off)
{
	return ((uint32_t)R(off) << 16 | R(off + 2)) & ~1u;
}

static void setptr(int off, uint32_t v)
{
	R(off) = v >> 16;
	R(off + 2) = v;
}

static uint16_t rd(uint32_t a)
{
	const uint8_t *p = PTR(a);
	return p[0] << 8 | p[1];
}

static void wr(uint32_t a, uint16_t v)
{
	uint8_t *p = PTR(a);
	p[0] = v >> 8;
	p[1] = v;
}

static uint16_t minterm(int mt, uint16_t a, uint16_t b, uint16_t c)
{
	uint16_t d = 0;

	if (mt & 0x01) d |= ~a & ~b & ~c;
	if (mt & 0x02) d |= ~a & ~b & c;
	if (mt & 0x04) d |= ~a & b & ~c;
	if (mt & 0x08) d |= ~a & b & c;
	if (mt & 0x10) d |= a & ~b & ~c;
	if (mt & 0x20) d |= a & ~b & c;
	if (mt & 0x40) d |= a & b & ~c;
	if (mt & 0x80) d |= a & b & c;
	return d;
}

static void blit_area(int w, int h)
{
	uint16_t con0 = R(0x40), con1 = R(0x42);
	int ash = con0 >> 12, bsh = con1 >> 12, mt = con0 & 0xff;
	int usea = con0 & 0x800, useb = con0 & 0x400, usec = con0 & 0x200, used = con0 & 0x100;
	int desc = con1 & 2, ife = con1 & 8, efe = con1 & 0x10, fci = (con1 >> 2) & 1;
	int step = desc ? -2 : 2, x, y;
	uint32_t apt = ptr(0x50), bpt = ptr(0x4c), cpt = ptr(0x48), dpt = ptr(0x54);
	int16_t amod = R(0x64), bmod = R(0x62), cmod = R(0x60), dmod = R(0x66);
	uint16_t adat = R(0x74), bdat = R(0x72), cdat = R(0x70);
	uint16_t aprev = 0, bprev = 0, d = 0;

	for (y = 0; y < h; y++) {
		int carry = fci;

		for (x = 0; x < w; x++) {
			uint16_t a, b, as, bs;

			if (usea) {
				adat = rd(apt);
				apt += step;
			}
			if (useb) {
				bdat = rd(bpt);
				bpt += step;
			}
			if (usec) {
				cdat = rd(cpt);
				cpt += step;
			}
			a = adat;
			if (x == 0)
				a &= R(0x44);
			if (x == w - 1)
				a &= R(0x46);
			b = bdat;
			if (desc) {
				as = (uint16_t)(((uint32_t)a << ash | aprev >> (16 - ash)) & 0xffff);
				bs = (uint16_t)(((uint32_t)b << bsh | bprev >> (16 - bsh)) & 0xffff);
			} else {
				as = (uint16_t)(((uint32_t)aprev << 16 | a) >> ash);
				bs = (uint16_t)(((uint32_t)bprev << 16 | b) >> bsh);
			}
			aprev = a;
			bprev = b;
			d = minterm(mt, as, bs, cdat);
			if (ife || efe) {       /* area fill, right to left */
				int bit;
				uint16_t out = 0;
				for (bit = 0; bit < 16; bit++) {
					int v = (d >> bit) & 1;
					if (efe) {
						carry ^= v;
						out |= carry << bit;
					} else {
						out |= (v | carry) << bit;
						carry ^= v;
					}
				}
				d = out;
			}
			if (used) {
				wr(dpt, d);
				dpt += step;
			}
		}
		if (usea) apt += desc ? -amod : amod;
		if (useb) bpt += desc ? -bmod : bmod;
		if (usec) cpt += desc ? -cmod : cmod;
		if (used) dpt += desc ? -dmod : dmod;
	}
	setptr(0x50, apt);
	setptr(0x4c, bpt);
	setptr(0x48, cpt);
	setptr(0x54, dpt);
	R(0x74) = adat;
	R(0x72) = bdat;
	R(0x70) = cdat;
	R(0x00) = d;                            /* BLTDDAT */
}

/*
 * Line mode (HRM "Line Mode"): BLTAPTL the error, BLTAMOD 4(dy-dx) and
 * BLTBMOD 4dy its steps, the octant in BLTCON1 (SUD, SUL, AUL), SIGN the
 * error's first sign, SING one dot a row; ASH in BLTCON0 the start x
 * within the word, BLTADAT $8000, BLTBDAT the texture (rotated by BSH);
 * C and D point at the first word, CMOD/DMOD the bytes a row; BLTSIZE's
 * height the length.
 */
static void blit_line(int len)
{
	uint16_t con0 = R(0x40), con1 = R(0x42);
	int ash = con0 >> 12, mt = con0 & 0xff;
	int sud = con1 & 0x10, sul = con1 & 0x08, aul = con1 & 0x04, sing = con1 & 0x02;
	int sign = con1 & 0x40, i, dotted = 0;
	int16_t err = (int16_t)R(0x52);         /* BLTAPTL */
	int16_t amod = R(0x64), bmod = R(0x62), cmod = R(0x60);
	uint32_t cpt = ptr(0x48), dpt = ptr(0x54);
	uint16_t tex = R(0x72);
	int bsh = con1 >> 12;

	(void)dpt;
	for (i = 0; i < len; i++) {
		uint16_t a = 0x8000 >> ash;
		uint16_t b = (tex >> bsh) & 1 ? 0xffff : 0;
		uint16_t c = rd(cpt), d;
		int xstep = 0, ystep = 0;

		if (sing && dotted)
			a = 0;
		d = minterm(mt, a, b, c);
		wr(cpt, d);
		dotted = 1;
		bsh = (bsh - 1) & 15;
		/* the octant: the major axis steps every time, the minor one
		   when the error says so */
		if (!sign) {                    /* error >= 0: a minor step too */
			err += amod;
			if (sud) {
				ystep = sul ? -1 : 1;
				xstep = aul ? -1 : 1;
			} else {
				xstep = sul ? -1 : 1;
				ystep = aul ? -1 : 1;
			}
		} else {
			err += bmod;
			if (sud)
				ystep = sul ? -1 : 1;
			else
				xstep = sul ? -1 : 1;
		}
		sign = err < 0;
		if (ystep) {
			cpt += ystep * cmod;
			dotted = 0;
		}
		if (xstep > 0 && ++ash == 16) {
			ash = 0;
			cpt += 2;
		} else if (xstep < 0 && --ash < 0) {
			ash = 15;
			cpt -= 2;
		}
	}
	setptr(0x48, cpt);
	setptr(0x54, cpt);
	R(0x52) = err;
	R(0x40) = (con0 & 0x0fff) | ash << 12;
}

static void start(int w, int h)
{
	if (R(0x42) & 1)
		blit_line(h);
	else
		blit_area(w, h);
}

/* a write of `size` bytes at offset off of the custom chips */
int blitter_write(uint32_t off, int size, uint32_t v)
{
	if (size == 4) {
		blitter_write(off, 2, v >> 16);
		return blitter_write(off + 2, 2, v & 0xffff);
	}
	if (size == 1)
		v = off & 1 ? (R(off & ~1u) & 0xff00) | (v & 0xff) :
			      (R(off) & 0x00ff) | (v & 0xff) << 8;
	off &= 0x1fe;
	if (off < 0x40 || off > 0x74)
		return 0;
	R(off) = v;
	if (off == 0x58)                        /* BLTSIZE */
		start((v & 63) ? (v & 63) : 64, (v >> 6) ? (v >> 6) : 1024);
	else if (off == 0x5e)                   /* BLTSIZH (ECS), BLTSIZV first */
		start((v & 0x7ff) ? (v & 0x7ff) : 2048, R(0x5c) & 0x7fff ? R(0x5c) & 0x7fff : 32768);
	return 1;
}

uint16_t blitter_read(uint32_t off)
{
	if ((off & 0x1fe) == 0x000)
		return R(0x00);                 /* BLTDDAT */
	return 0;
}
