/*
 * AmigaOS time: days since 1 Jan 1978, minutes, ticks (50/s), local time.
 * Also dos.library's DateToStr/StrToDate/CompareDates.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include "af.h"

#define EPOCH_DIFF 252460800    /* 1978-01-01 - 1970-01-01, seconds */

static long tzoff(int64_t t)
{
	time_t tt = t;
	struct tm tm;

	localtime_r(&tt, &tm);
	return tm.tm_gmtoff;
}

void date_from_unix(int64_t t, struct DateStamp *ds)
{
	int64_t s = t + tzoff(t) - EPOCH_DIFF;

	if (s < 0)
		s = 0;
	ds->ds_Days = s / 86400;
	ds->ds_Minute = (s % 86400) / 60;
	ds->ds_Tick = (s % 60) * 50;
}

void date_now(struct DateStamp *ds)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	date_from_unix(tv.tv_sec, ds);
	ds->ds_Tick += tv.tv_usec / 20000;
}

int64_t date_to_unix(const struct DateStamp *ds)
{
	int64_t s = (int64_t)ds->ds_Days * 86400 + ds->ds_Minute * 60 +
		    ds->ds_Tick / 50 + EPOCH_DIFF;

	return s - tzoff(s);
}

/* ---- DateToStr / StrToDate ---- */

static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
			      "Thursday", "Friday", "Saturday" };
static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
				"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

#define FORMAT_DOS 0
#define FORMAT_INT 1
#define FORMAT_USA 2
#define FORMAT_CDN 3
#define DTF_SUBST  1
#define DTF_FUTURE 2

static void d_DateToStr(struct frame *f)
{
	struct DateTime *dt = PTR(D(1));
	struct DateStamp now;
	int64_t s = (int64_t)dt->dat_Stamp.ds_Days * 86400 +
		    dt->dat_Stamp.ds_Minute * 60 + dt->dat_Stamp.ds_Tick / 50;
	time_t t = s + EPOCH_DIFF;
	struct tm tm;
	char date[32], *day = (char *)days[0];

	gmtime_r(&t, &tm);
	day = (char *)days[tm.tm_wday];
	switch (dt->dat_Format) {
	case FORMAT_INT:
		snprintf(date, sizeof(date), "%02d-%02d-%02d", tm.tm_year % 100,
			 tm.tm_mon + 1, tm.tm_mday);
		break;
	case FORMAT_USA:
		snprintf(date, sizeof(date), "%02d-%02d-%02d", tm.tm_mon + 1,
			 tm.tm_mday, tm.tm_year % 100);
		break;
	case FORMAT_CDN:
		snprintf(date, sizeof(date), "%02d-%02d-%02d", tm.tm_mday,
			 tm.tm_mon + 1, tm.tm_year % 100);
		break;
	default:
		snprintf(date, sizeof(date), "%02d-%s-%02d", tm.tm_mday,
			 months[tm.tm_mon], tm.tm_year % 100);
	}
	if (dt->dat_Flags & DTF_SUBST) {
		int32_t diff;
		date_now(&now);
		diff = dt->dat_Stamp.ds_Days - now.ds_Days;
		if (diff == 0)
			strcpy(date, "Today");
		else if (diff == -1)
			strcpy(date, "Yesterday");
		else if (diff == 1)
			strcpy(date, "Tomorrow");
		else if (diff < 0 && diff > -7)
			snprintf(date, sizeof(date), "%s", day);
	}
	if (dt->dat_StrDay)
		strcpy(PTR(dt->dat_StrDay), day);
	if (dt->dat_StrDate)
		strcpy(PTR(dt->dat_StrDate), date);
	if (dt->dat_StrTime)
		sprintf(PTR(dt->dat_StrTime), "%02d:%02d:%02d", tm.tm_hour,
			tm.tm_min, tm.tm_sec);
	RET(DOSTRUE);
}

static int64_t days_from_civil(int y, int m, int d)
{
	struct tm tm = { 0 };

	tm.tm_year = y - 1900;
	tm.tm_mon = m - 1;
	tm.tm_mday = d;
	tm.tm_hour = 12;
	return (timegm(&tm) - EPOCH_DIFF) / 86400;
}

static void d_StrToDate(struct frame *f)
{
	struct DateTime *dt = PTR(D(1));
	struct DateStamp now;
	int64_t day = -1;

	date_now(&now);
	if (dt->dat_StrDate) {
		const char *s = PTR(dt->dat_StrDate);
		int a, b, c, i;
		char mon[8];
		if (!strcasecmp(s, "today"))
			day = now.ds_Days;
		else if (!strcasecmp(s, "yesterday"))
			day = now.ds_Days - 1;
		else if (!strcasecmp(s, "tomorrow"))
			day = now.ds_Days + 1;
		for (i = 0; i < 7 && day < 0; i++)
			if (!strcasecmp(s, days[i])) {
				int wd = (now.ds_Days + 0) % 7; /* 1978-01-01 = Sunday */
				int back = (wd - i + 7) % 7;
				if (dt->dat_Flags & DTF_FUTURE)
					day = now.ds_Days + ((i - wd + 7) % 7);
				else
					day = now.ds_Days - back;
			}
		if (day < 0 && dt->dat_Format == FORMAT_DOS &&
		    sscanf(s, "%d-%3[A-Za-z]-%d", &a, mon, &c) == 3) {
			for (i = 0; i < 12; i++)
				if (!strncasecmp(mon, months[i], 3))
					break;
			if (i < 12) {
				c += c < 78 ? 2000 : c < 100 ? 1900 : 0;
				day = days_from_civil(c, i + 1, a);
			}
		} else if (day < 0 && sscanf(s, "%d-%d-%d", &a, &b, &c) == 3) {
			int y, m, d;
			switch (dt->dat_Format) {
			case FORMAT_INT: y = a; m = b; d = c; break;
			case FORMAT_USA: m = a; d = b; y = c; break;
			default: d = a; m = b; y = c; break;
			}
			y += y < 78 ? 2000 : y < 100 ? 1900 : 0;
			day = days_from_civil(y, m, d);
		}
		if (day < 0) {
			RET(DOSFALSE);
			return;
		}
		dt->dat_Stamp.ds_Days = day;
	}
	if (dt->dat_StrTime) {
		int h = 0, m = 0, s = 0;
		if (sscanf(PTR(dt->dat_StrTime), "%d:%d:%d", &h, &m, &s) < 2 ||
		    h > 23 || m > 59 || s > 59) {
			RET(DOSFALSE);
			return;
		}
		dt->dat_Stamp.ds_Minute = h * 60 + m;
		dt->dat_Stamp.ds_Tick = s * 50;
	}
	RET(DOSTRUE);
}

static void d_CompareDates(struct frame *f)
{
	struct DateStamp *a = PTR(D(1)), *b = PTR(D(2));
	int32_t r = b->ds_Days - a->ds_Days;

	if (!r)
		r = b->ds_Minute - a->ds_Minute;
	if (!r)
		r = b->ds_Tick - a->ds_Tick;
	RET(r < 0 ? -1 : r > 0);
}

const struct libimpl dos_impl_date[] = {
	{ "DateToStr", d_DateToStr }, { "StrToDate", d_StrToDate },
	{ "CompareDates", d_CompareDates },
	{ NULL, NULL }
};
