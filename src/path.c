/*
 * AmigaOS paths onto Linux paths.
 *
 *   NAME:rest   NAME is an assign (case-insensitive), configurable
 *   :rest       the root of SYS:
 *   rest        relative to the current directory
 *   leading /   each one is the parent; "a//b" is "a/../b"
 *
 * Assigns come from built-in defaults, /etc/amigafmt.conf, ~/.amigafmtrc
 * (or $AMIGAFMT_CONF) and -a NAME=path, later ones winning.  A target may
 * itself start with an assign ("C: SYS:C").  Components are looked up
 * case-insensitively when the exact name does not exist, as AmigaOS
 * filesystems are case-insensitive.
 */
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "af.h"
#include "dos_int.h"

struct assign {
	char *name;             /* without the colon */
	char *target;           /* Amiga or Linux path */
};

static struct assign assigns[64];
static int nassigns;
static char cwd[PATH_MAX];
int path_unix;                  /* leading '/' is the Linux root */

int path_assign(const char *name, const char *target)
{
	char n[64];
	size_t l = strlen(name);
	int i;

	if (l && name[l - 1] == ':')
		l--;
	if (!l || l >= sizeof(n))
		return 0;
	memcpy(n, name, l);
	n[l] = 0;
	for (i = 0; i < nassigns; i++)
		if (!strcasecmp(assigns[i].name, n)) {
			free(assigns[i].target);
			assigns[i].target = strdup(target);
			return 1;
		}
	if (nassigns == 64)
		return 0;
	assigns[nassigns].name = strdup(n);
	assigns[nassigns].target = strdup(target);
	nassigns++;
	return 1;
}

static void read_conf(const char *file)
{
	FILE *fp = fopen(file, "r");
	char line[1024];

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *p = line, *name, *tgt, *e;
		while (isspace((unsigned char)*p))
			p++;
		if (!*p || *p == '#' || *p == ';')
			continue;
		if (!strncasecmp(p, "unixpaths", 9)) {
			path_unix = !strstr(p + 9, "no") && !strstr(p + 9, "off");
			continue;
		}
		if (!strncasecmp(p, "assign", 6) && isspace((unsigned char)p[6]))
			for (p += 6; isspace((unsigned char)*p); p++)
				;
		name = p;
		while (*p && !isspace((unsigned char)*p) && *p != '=')
			p++;
		if (!*p)
			continue;
		*p++ = 0;
		while (isspace((unsigned char)*p) || *p == '=')
			p++;
		tgt = p;
		e = tgt + strlen(tgt);
		while (e > tgt && isspace((unsigned char)e[-1]))
			*--e = 0;
		if (*tgt)
			path_assign(name, tgt);
	}
	fclose(fp);
}

void path_init(const char *progpath)
{
	char buf[PATH_MAX], *p;
	const char *home = getenv("HOME"), *conf = getenv("AMIGAFMT_CONF");

	if (!getcwd(cwd, sizeof(cwd)))
		strcpy(cwd, "/");
	path_assign("SYS", "/");
	path_assign("ROOT", "/");
	path_assign("C", "SYS:C");
	path_assign("S", "SYS:S");
	path_assign("L", "SYS:L");
	path_assign("LIBS", "SYS:Libs");
	path_assign("DEVS", "SYS:Devs");
	path_assign("FONTS", "SYS:Fonts");
	path_assign("LOCALE", "SYS:Locale");
	path_assign("ENVARC", "SYS:Prefs/Env-Archive");
	path_assign("ENV", "RAM:Env");
	path_assign("T", "/tmp");
	path_assign("RAM", "/tmp");
	path_assign("HOME", home ? home : "/");
	read_conf("/etc/amigafmt.conf");
	if (conf)
		read_conf(conf);
	else if (home) {
		snprintf(buf, sizeof(buf), "%s/.amigafmtrc", home);
		read_conf(buf);
	}
	if (getenv("AMIGAFMT_SYS"))
		path_assign("SYS", getenv("AMIGAFMT_SYS"));
	if (getenv("AMIGAFMT_UNIXPATHS"))
		path_unix = 1;
	if (progpath && realpath(progpath, buf) && (p = strrchr(buf, '/'))) {
		*p = 0;
		path_assign("PROGDIR", *buf ? buf : "/");
	}
}

void path_dump(FILE *f)
{
	for (int i = 0; i < nassigns; i++)
		fprintf(f, "%s: %s\n", assigns[i].name, assigns[i].target);
}

const char *path_cwd(void)
{
	return cwd;
}

void path_set_cwd(const char *lpath)
{
	snprintf(cwd, sizeof(cwd), "%s", lpath);
	if (chdir(cwd)) {
		/* keep going: every path is made absolute anyway */
	}
}

/* append one component to an absolute Linux path, case-insensitively */
static void push(char *out, const char *comp, size_t clen)
{
	char dir[PATH_MAX], name[NAME_MAX + 1];
	const char *sep;
	struct stat st;
	DIR *d;
	struct dirent *e;

	if (clen > NAME_MAX)
		clen = NAME_MAX;
	memcpy(name, comp, clen);
	name[clen] = 0;
	strcpy(dir, out);
	sep = strcmp(dir, "/") ? "/" : "";
	if (strlen(dir) + clen + 2 >= PATH_MAX)
		return;
	sprintf(out, "%s%s%s", dir, sep, name);
	if (!lstat(out, &st) || !(d = opendir(dir)))
		return;
	while ((e = readdir(d)))
		if (!strcasecmp(e->d_name, name)) {
			sprintf(out, "%s%s%s", dir, sep, e->d_name);
			break;
		}
	closedir(d);
}

static void pop(char *out)
{
	char *s = strrchr(out, '/');

	if (!s || s == out)
		strcpy(out, "/");
	else
		*s = 0;
}

static int resolve(const char *apath, const char *base, char *out, int depth);

/* the Linux directory an assign names */
static int assign_dir(const char *dev, size_t dlen, char *out, int depth)
{
	int i;

	for (i = 0; i < nassigns; i++)
		if (strlen(assigns[i].name) == dlen &&
		    !strncasecmp(assigns[i].name, dev, dlen)) {
			const char *t = assigns[i].target;
			if (t[0] == '/') {
				snprintf(out, PATH_MAX, "%s", t);
				return 1;
			}
			return resolve(t, "/", out, depth + 1);
		}
	return 0;
}

static int resolve(const char *apath, const char *base, char *out, int depth)
{
	const char *p = apath, *colon = strchr(apath, ':');

	if (depth > 8)
		return 0;
	if (colon) {
		if (colon == apath) {
			if (!assign_dir("SYS", 3, out, depth))
				strcpy(out, "/");
		} else if (!assign_dir(apath, colon - apath, out, depth))
			return 0;
		p = colon + 1;
	} else if (path_unix && *p == '/') {
		strcpy(out, "/");
		while (*p == '/')
			p++;
	} else {
		snprintf(out, PATH_MAX, "%s", base);
	}
	/* leading slashes: one parent each */
	while (*p == '/') {
		pop(out);
		p++;
	}
	while (*p) {
		const char *s = strchr(p, '/');
		size_t n = s ? (size_t)(s - p) : strlen(p);
		if (n)
			push(out, p, n);
		p += n;
		if (*p == '/') {
			p++;
			while (*p == '/') {     /* "a//b": parent */
				pop(out);
				p++;
			}
		}
	}
	return 1;
}

/* malloc()ed Linux path, or NULL when the device is unknown */
char *path_to_linux(const char *apath, const char *base)
{
	char out[PATH_MAX];

	if (!apath)
		apath = "";
	if (!base)
		base = me ? lock_path(me->pr_CurrentDir) : cwd;
	if (!resolve(apath, base, out, 0))
		return NULL;
	return strdup(out);
}

/*
 * The Amiga name for a Linux path.  Only assigns to Linux directories
 * (volumes, in effect) are candidates, the longest match wins, and on a
 * tie the earlier one in the table: SYS, then RAM, then the rest.
 */
char *path_to_amiga(const char *lpath)
{
	static const char *skip[] = { "PROGDIR", "HOME", "ENV", "T", NULL };
	char *res;
	size_t best = 0, l;
	int bi = -1, i, k;

	for (i = 0; i < nassigns; i++) {
		const char *dir = assigns[i].target;
		if (dir[0] != '/')
			continue;
		for (k = 0; skip[k]; k++)
			if (!strcasecmp(assigns[i].name, skip[k]))
				break;
		if (skip[k])
			continue;
		l = strlen(dir);
		while (l > 1 && dir[l - 1] == '/')
			l--;
		if (l == 1)
			l = 0;          /* "/" matches everything */
		if (!strncmp(lpath, dir, l) && (lpath[l] == '/' || !lpath[l]) &&
		    (bi < 0 || l > best)) {
			best = l;
			bi = i;
		}
	}
	if (bi < 0)
		return strdup(lpath);
	lpath += best;
	while (*lpath == '/')
		lpath++;
	res = malloc(strlen(assigns[bi].name) + strlen(lpath) + 2);
	sprintf(res, "%s:%s", assigns[bi].name, lpath);
	return res;
}
