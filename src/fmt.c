/*
 * The RawDoFmt() format engine, also behind dos.library's VPrintf family.
 *
 *   %[-][0][width][.limit][l]{d,u,x,X,s,b,c}  and %%
 *
 * Without 'l' a number or character is a 16-bit WORD in the data stream;
 * with it a LONG.  %s takes a C string pointer, %b a BSTR (as a BPTR).
 * Hex is upper case.  Returns the data pointer after the last argument.
 */
#include <string.h>
#include "af.h"

static uint32_t rd32(uint32_t *p)
{
	const uint8_t *b = PTR(*p);
	*p += 4;
	return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

static uint32_t rd16(uint32_t *p)
{
	const uint8_t *b = PTR(*p);
	*p += 2;
	return b[0] << 8 | b[1];
}

uint32_t fmt_do(const char *fmt, uint32_t data, putch_fn put, void *ctx,
		int unused)
{
	(void)unused;
	for (; *fmt; fmt++) {
		int left = 0, zero = 0, width = 0, limit = -1, lng = 0;
		char buf[40], *s;
		int len, neg = 0;
		uint32_t v;

		if (*fmt != '%') {
			put(*fmt, ctx);
			continue;
		}
		fmt++;
		if (*fmt == '-')
			left = 1, fmt++;
		if (*fmt == '0')
			zero = 1, fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + *fmt++ - '0';
		if (*fmt == '.') {
			limit = 0;
			for (fmt++; *fmt >= '0' && *fmt <= '9'; fmt++)
				limit = limit * 10 + *fmt - '0';
		}
		if (*fmt == 'l')
			lng = 1, fmt++;
		s = buf;
		switch (*fmt) {
		case 'd':
		case 'D':
		case 'u':
		case 'U':
		case 'x':
		case 'X':
			if (lng || *fmt == 'D' || *fmt == 'U')
				v = rd32(&data);
			else {
				v = rd16(&data);
				if (*fmt == 'd')
					v = (uint32_t)(int32_t)(int16_t)v;
			}
			if ((*fmt == 'd' || *fmt == 'D') && (int32_t)v < 0) {
				neg = 1;
				v = -v;
			}
			{
				char tmp[16];
				int n = 0, base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
				do {
					tmp[n++] = "0123456789ABCDEF"[v % base];
					v /= base;
				} while (v);
				len = 0;
				if (neg)
					buf[len++] = '-';
				while (n)
					buf[len++] = tmp[--n];
				buf[len] = 0;
			}
			break;
		case 's':
			v = rd32(&data);
			s = v ? PTR(v) : "";
			len = strlen(s);
			if (limit >= 0 && len > limit)
				len = limit;
			break;
		case 'b':
			v = rd32(&data);
			if (v) {
				const uint8_t *b = BADDR(v);
				s = (char *)b + 1;
				len = b[0];
			} else {
				s = "";
				len = 0;
			}
			if (limit >= 0 && len > limit)
				len = limit;
			break;
		case 'c':
			v = lng ? rd32(&data) : rd16(&data);
			buf[0] = v;
			len = 1;
			break;
		case '%':
			buf[0] = '%';
			len = 1;
			break;
		case 0:
			return data;
		default:
			buf[0] = *fmt;
			len = 1;
			break;
		}
		if (zero && !left && neg) {
			put('-', ctx);
			s++, len--, width--;
		}
		if (!left)
			for (int i = len; i < width; i++)
				put(zero ? '0' : ' ', ctx);
		for (int i = 0; i < len; i++)
			put((unsigned char)s[i], ctx);
		if (left)
			for (int i = len; i < width; i++)
				put(' ', ctx);
	}
	return data;
}
