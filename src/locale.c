/*
 * locale.library, English only: OpenLocale gives a fixed English locale,
 * OpenCatalog finds no catalogs (so every GetCatalogStr is the program's
 * own built-in string, which is English), FormatString is RawDoFmt
 * through the caller's hook, FormatDate does the locale %-codes, the
 * character classes are Latin-1.  Enough for programs that are
 * localised but run happily in their own language.
 */
#include <ctype.h>
#include <string.h>
#include <time.h>
#include "af.h"

static const char *const strs[] = {
	NULL, "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "January",
	"February", "March", "April", "May", "June", "July", "August",
	"September", "October", "November", "December", "Jan", "Feb", "Mar",
	"Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec", "Yes",
	"No", "AM", "PM", "-", "-", "\"", "\"", "Yesterday", "Today",
	"Tomorrow", "Future",
};

/* struct Locale (libraries/locale.h), 0xa4 bytes */
static uint8_t *the_locale;

static char *astr(const char *s)
{
	char *p = amem_alloc(strlen(s) + 1);
	strcpy(p, s);
	return p;
}

static uint8_t *locale_get(void)
{
	static const uint8_t grouping[2] = { 3, 0 };
	uint8_t *l;
	uint8_t *g;

	if (the_locale)
		return the_locale;
	l = amem_alloc(0xa8);
	g = amem_alloc(2);
	memcpy(g, grouping, 2);
	*(uint32_t *)(l + 0) = APTR_OF(astr("english"));        /* loc_LocaleName */
	*(uint32_t *)(l + 4) = APTR_OF(astr("english"));        /* loc_LanguageName */
	*(uint32_t *)(l + 8) = APTR_OF(astr("english"));        /* loc_PrefLanguages[0] */
	*(uint32_t *)(l + 0x38) = 44;                           /* loc_CountryCode: UK */
	*(uint32_t *)(l + 0x3c) = 44;                           /* loc_TelephoneCode */
	l[0x44] = 0;                                            /* loc_MeasuringSystem: MS_ISO */
	*(uint32_t *)(l + 0x48) = APTR_OF(astr("%A %e-%b-%Y %H:%M:%S"));	/* DateTimeFormat */
	*(uint32_t *)(l + 0x4c) = APTR_OF(astr("%A %e-%b-%Y"));		/* DateFormat */
	*(uint32_t *)(l + 0x50) = APTR_OF(astr("%H:%M:%S"));		/* TimeFormat */
	*(uint32_t *)(l + 0x54) = APTR_OF(astr("%d-%b-%y %H:%M:%S"));	/* ShortDateTimeFormat */
	*(uint32_t *)(l + 0x58) = APTR_OF(astr("%d-%b-%y"));		/* ShortDateFormat */
	*(uint32_t *)(l + 0x5c) = APTR_OF(astr("%H:%M"));		/* ShortTimeFormat */
	*(uint32_t *)(l + 0x60) = APTR_OF(astr("."));           /* DecimalPoint */
	*(uint32_t *)(l + 0x64) = APTR_OF(astr(","));           /* GroupSeparator */
	*(uint32_t *)(l + 0x68) = APTR_OF(astr(""));            /* FracGroupSeparator */
	*(uint32_t *)(l + 0x6c) = APTR_OF(g);                   /* Grouping */
	*(uint32_t *)(l + 0x70) = APTR_OF(g);                   /* FracGrouping */
	*(uint32_t *)(l + 0x74) = APTR_OF(astr("."));           /* MonDecimalPoint */
	*(uint32_t *)(l + 0x78) = APTR_OF(astr(","));
	*(uint32_t *)(l + 0x7c) = APTR_OF(astr(""));
	*(uint32_t *)(l + 0x80) = APTR_OF(g);
	*(uint32_t *)(l + 0x84) = APTR_OF(g);
	l[0x88] = 2;                                            /* MonFracDigits */
	l[0x89] = 2;                                            /* MonIntFracDigits */
	*(uint32_t *)(l + 0x8c) = APTR_OF(astr("\xa3"));       /* MonCS */
	*(uint32_t *)(l + 0x90) = APTR_OF(astr("p"));           /* MonSmallCS */
	*(uint32_t *)(l + 0x94) = APTR_OF(astr("GBP"));         /* MonIntCS */
	*(uint32_t *)(l + 0x98) = APTR_OF(astr(""));            /* MonPositiveSign */
	*(uint32_t *)(l + 0xa0) = APTR_OF(astr("-"));           /* MonNegativeSign */
	the_locale = l;
	return l;
}

static void lo_OpenLocale(struct frame *f) { RET(locale_get()); }
static void lo_Nop(struct frame *f) { (void)f; }
static void lo_Zero(struct frame *f) { RET(0); }

static void lo_GetCatalogStr(struct frame *f) { RET(A(1)); }

static void lo_GetLocaleStr(struct frame *f)
{
	uint32_t n = D(0);
	static uint32_t cache[sizeof(strs) / sizeof(strs[0])];

	if (n == 0 || n >= sizeof(strs) / sizeof(strs[0])) {
		RET(0);
		return;
	}
	if (!cache[n])
		cache[n] = APTR_OF(astr(strs[n]));
	RET(cache[n]);
}

/* Latin-1 classes */
static int l1alpha(int c) { return isalpha(c) || (c >= 0xc0 && c != 0xd7 && c != 0xf7); }
static int l1upper(int c) { return isupper(c) || (c >= 0xc0 && c <= 0xde && c != 0xd7); }
static int l1lower(int c) { return islower(c) || (c >= 0xdf && c != 0xf7); }

static void lo_ConvToLower(struct frame *f)
{
	int c = D(0) & 0xff;
	RET(l1upper(c) ? c + 32 : c);
}

static void lo_ConvToUpper(struct frame *f)
{
	int c = D(0) & 0xff;
	RET(l1lower(c) && c != 0xdf && c != 0xff ? c - 32 : c);
}

#define CLASS(name, expr) \
	static void lo_##name(struct frame *f) { int c = D(0) & 0xff; RET((expr) ? 1 : 0); }
CLASS(IsAlNum, l1alpha(c) || isdigit(c))
CLASS(IsAlpha, l1alpha(c))
CLASS(IsCntrl, c < 32 || (c >= 127 && c < 160))
CLASS(IsDigit, isdigit(c))
CLASS(IsGraph, (c > 32 && c < 127) || c > 160)
CLASS(IsLower, l1lower(c))
CLASS(IsPrint, (c >= 32 && c < 127) || c >= 160)
CLASS(IsPunct, ispunct(c) || (c > 160 && c < 192) || c == 0xd7 || c == 0xf7)
CLASS(IsSpace, isspace(c) || c == 160)
CLASS(IsUpper, l1upper(c))
CLASS(IsXDigit, isxdigit(c))

/* the caller's hook: a0 hook, a1 the character, a2 the locale */
struct hookout {
	struct frame *f;
	uint32_t hook, locale;
};

static void hook_put(int c, void *ctx)
{
	struct hookout *h = ctx;
	struct regs r;

	memset(&r, 0, sizeof(r));
	r.a[0] = h->hook;
	r.a[1] = (uint8_t)c;
	r.a[2] = h->locale;
	amiga_call(h->f, *(uint32_t *)((uint8_t *)PTR(h->hook) + 8), &r);
}

static void lo_FormatString(struct frame *f)
{
	struct hookout h = { f, A(3), A(0) };

	RET(fmt_do(PTR(A(1)), A(2), hook_put, &h, 0));
	hook_put(0, &h);
}

static void put_str(struct hookout *h, const char *s)
{
	while (*s)
		hook_put((uint8_t)*s++, h);
}

static void lo_FormatDate(struct frame *f)
{
	struct hookout h = { f, A(3), A(0) };
	const char *t = PTR(A(1));
	const struct DateStamp *ds = PTR(A(2));
	time_t secs = (time_t)date_to_unix(ds);
	struct tm tm;
	char b[64];

	gmtime_r(&secs, &tm);
	for (; *t; t++) {
		if (*t != '%' || !t[1]) {
			hook_put((uint8_t)*t, &h);
			continue;
		}
		t++;
		b[0] = 0;
		switch (*t) {
		case 'a': put_str(&h, strs[8 + tm.tm_wday]); continue;
		case 'A': put_str(&h, strs[1 + tm.tm_wday]); continue;
		case 'b': case 'h': put_str(&h, strs[27 + tm.tm_mon]); continue;
		case 'B': put_str(&h, strs[15 + tm.tm_mon]); continue;
		case 'c': strftime(b, sizeof(b), "%a %b %d %H:%M:%S %Y", &tm); break;
		case 'C': strftime(b, sizeof(b), "%a %b %e %T %Z %Y", &tm); break;
		case 'd': snprintf(b, sizeof(b), "%02d", tm.tm_mday); break;
		case 'D': strftime(b, sizeof(b), "%m/%d/%y", &tm); break;
		case 'e': snprintf(b, sizeof(b), "%2d", tm.tm_mday); break;
		case 'H': snprintf(b, sizeof(b), "%02d", tm.tm_hour); break;
		case 'I': snprintf(b, sizeof(b), "%02d", (tm.tm_hour + 11) % 12 + 1); break;
		case 'j': snprintf(b, sizeof(b), "%03d", tm.tm_yday + 1); break;
		case 'm': snprintf(b, sizeof(b), "%02d", tm.tm_mon + 1); break;
		case 'M': snprintf(b, sizeof(b), "%02d", tm.tm_min); break;
		case 'n': b[0] = '\n', b[1] = 0; break;
		case 'p': put_str(&h, tm.tm_hour < 12 ? "AM" : "PM"); continue;
		case 'q': snprintf(b, sizeof(b), "%d", tm.tm_hour); break;
		case 'Q': snprintf(b, sizeof(b), "%d", (tm.tm_hour + 11) % 12 + 1); break;
		case 'r': strftime(b, sizeof(b), "%I:%M:%S %p", &tm); break;
		case 'R': strftime(b, sizeof(b), "%H:%M", &tm); break;
		case 'S': snprintf(b, sizeof(b), "%02d", tm.tm_sec); break;
		case 't': b[0] = '\t', b[1] = 0; break;
		case 'T': strftime(b, sizeof(b), "%H:%M:%S", &tm); break;
		case 'U': strftime(b, sizeof(b), "%U", &tm); break;
		case 'w': snprintf(b, sizeof(b), "%d", tm.tm_wday); break;
		case 'W': strftime(b, sizeof(b), "%W", &tm); break;
		case 'x': strftime(b, sizeof(b), "%m/%d/%y", &tm); break;
		case 'X': strftime(b, sizeof(b), "%H:%M:%S", &tm); break;
		case 'y': snprintf(b, sizeof(b), "%02d", tm.tm_year % 100); break;
		case 'Y': snprintf(b, sizeof(b), "%d", tm.tm_year + 1900); break;
		default: b[0] = *t, b[1] = 0; break;
		}
		put_str(&h, b);
	}
	hook_put(0, &h);
}

static void lo_StrnCmp(struct frame *f)
{
	const uint8_t *a = PTR(A(1)), *b = PTR(A(2));
	int32_t n = D(0), i;

	for (i = 0; n < 0 || i < n; i++) {
		int ca = a[i], cb = b[i];
		if (D(1) != 0) {        /* SC_COLLATE1/2: case-insensitive */
			if (l1upper(ca)) ca += 32;
			if (l1upper(cb)) cb += 32;
		}
		if (ca != cb) {
			RET(ca < cb ? -1 : 1);
			return;
		}
		if (!ca)
			break;
	}
	RET(0);
}

static void lo_StrConvert(struct frame *f)
{
	const char *s = PTR(A(1));
	char *buf = PTR(A(2));
	uint32_t n = strlen(s);

	if (n > D(0))
		n = D(0);
	memcpy(buf, s, n);
	RET(n);
}

static const struct libimpl locale_impl[] = {
	{ "CloseCatalog", lo_Nop }, { "CloseLocale", lo_Nop },
	{ "ConvToLower", lo_ConvToLower }, { "ConvToUpper", lo_ConvToUpper },
	{ "FormatDate", lo_FormatDate }, { "FormatString", lo_FormatString },
	{ "GetCatalogStr", lo_GetCatalogStr }, { "GetLocaleStr", lo_GetLocaleStr },
	{ "IsAlNum", lo_IsAlNum }, { "IsAlpha", lo_IsAlpha },
	{ "IsCntrl", lo_IsCntrl }, { "IsDigit", lo_IsDigit },
	{ "IsGraph", lo_IsGraph }, { "IsLower", lo_IsLower },
	{ "IsPrint", lo_IsPrint }, { "IsPunct", lo_IsPunct },
	{ "IsSpace", lo_IsSpace }, { "IsUpper", lo_IsUpper },
	{ "IsXDigit", lo_IsXDigit },
	{ "OpenCatalogA", lo_Zero }, { "OpenLocale", lo_OpenLocale },
	{ "ParseDate", lo_Zero },
	{ "StrConvert", lo_StrConvert }, { "StrnCmp", lo_StrnCmp },
	{ NULL, NULL }
};

struct amilib lib_locale = {
	.name = "locale.library", .version = 40, .revision = 10,
	.fd = fd_locale, .impl = locale_impl,
	.possize = 2,
};
