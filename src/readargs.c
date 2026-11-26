/*
 * ReadArgs(), ReadItem(), FindArg().
 *
 * The three passes follow amitools' vamos (lib/dos/Args.py): positional
 * items fill non-keyword slots in order, /M collects the rest, then /A
 * slots still empty take items back off the end of the /M list
 * ("FROM/M,TO/A" with "a b c" gives TO=c), then numbers are converted.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "af.h"
#include "dos_int.h"

#define ITEM_EQUAL    (-2)
#define ITEM_ERROR    (-1)
#define ITEM_NOTHING  0
#define ITEM_UNQUOTED 1
#define ITEM_QUOTED   2

struct src {
	const char *s;
	int32_t len, pos;
};

static int sgetc(struct src *x)
{
	return x->pos < x->len ? (unsigned char)x->s[x->pos++] : -1;
}

static void sunget(struct src *x)
{
	if (x->pos > 0)
		x->pos--;
}

/* returns an ITEM_ code; *out is malloc()ed for words */
static int read_item(struct src *x, char **out)
{
	char *b = malloc(x->len + 2);
	int n = 0, c;

	*out = b;
	b[0] = 0;
	do
		c = sgetc(x);
	while (c == ' ' || c == '\t');
	if (c < 0 || c == '\n') {
		if (c == '\n')
			sunget(x);
		return ITEM_NOTHING;
	}
	if (c == '=')
		return ITEM_EQUAL;
	if (c == '"') {
		for (;;) {
			c = sgetc(x);
			if (c < 0 || c == '\n')
				return ITEM_ERROR;
			if (c == '"')
				break;
			if (c == '*') {
				c = sgetc(x);
				if (c == 'n' || c == 'N')
					c = '\n';
				else if (c == 'e' || c == 'E')
					c = 0x1b;
				else if (c < 0 || c == '\n')
					return ITEM_ERROR;
			}
			b[n++] = c;
		}
		b[n] = 0;
		return ITEM_QUOTED;
	}
	while (c >= 0 && c != ' ' && c != '\t' && c != '\n' && c != '=') {
		b[n++] = c;
		c = sgetc(x);
	}
	if (c == '\n' || c == '=')
		sunget(x);
	b[n] = 0;
	return ITEM_UNQUOTED;
}

/* ---- the template ---- */

enum { T_STR, T_NUM, T_SWITCH, T_TOGGLE, T_FULL };

struct targ {
	char keys[8][32];
	int nkeys, type, keyword, required, multi;
};

static int parse_template(const char *t, struct targ *ta, int max)
{
	int n = 0;

	while (*t && n < max) {
		struct targ *a = &ta[n];
		const char *e = strchr(t, ',');
		size_t len = e ? (size_t)(e - t) : strlen(t);
		char item[256], *p, *k;

		memset(a, 0, sizeof(*a));
		if (len >= sizeof(item))
			len = sizeof(item) - 1;
		memcpy(item, t, len);
		item[len] = 0;
		t += len + (e != NULL);         /* an empty item is a slot too */
		p = strchr(item, '/');
		if (p)
			*p++ = 0;
		for (k = strtok(item, "="); k && a->nkeys < 8; k = strtok(NULL, "="))
			snprintf(a->keys[a->nkeys++], 32, "%s", k);
		while (p && *p) {
			switch (toupper((unsigned char)*p)) {
			case 'N': a->type = T_NUM; break;
			case 'S': a->type = T_SWITCH; a->keyword = 1; break;
			case 'T': a->type = T_TOGGLE; a->keyword = 1; break;
			case 'A': a->required = 1; break;
			case 'K': a->keyword = 1; break;
			case 'M': a->multi = 1; break;
			case 'F': a->type = T_FULL; break;
			}
			p = strchr(p, '/');
			if (p)
				p++;
		}
		n++;
	}
	return n;
}

static int find_key(struct targ *ta, int n, const char *w)
{
	for (int i = 0; i < n; i++)
		for (int k = 0; k < ta[i].nkeys; k++)
			if (!strcasecmp(ta[i].keys[k], w))
				return i;
	return -1;
}

int32_t ra_findarg(const char *tmpl, const char *key)
{
	struct targ ta[64];
	int n = parse_template(tmpl, ta, 64);
	return find_key(ta, n, key);
}

/* ---- result memory, hung off RDA_DAList ---- */

struct dablk {
	struct dablk *next;
	uint32_t pad[3];
};

static void *da_alloc(struct RDArgs *rd, size_t n)
{
	struct dablk *b = amem_alloc(sizeof(*b) + n + 1);

	b->next = PTR(rd->RDA_DAList);
	rd->RDA_DAList = (int32_t)APTR_OF(b);
	return b + 1;
}

static struct RDArgs *owned[32];

void ra_freeargs(struct RDArgs *rd)
{
	struct dablk *b, *n;
	int i;

	if (!rd)
		return;
	for (b = PTR(rd->RDA_DAList); b; b = n) {
		n = b->next;
		amem_free(b);
	}
	rd->RDA_DAList = 0;
	rd->RDA_Buffer = 0;
	for (i = 0; i < 32; i++)
		if (owned[i] == rd) {
			owned[i] = NULL;
			amem_free(rd);
		}
}

static int parse_number(const char *s, int32_t *v)
{
	char *e;
	long x;

	if (!*s)
		return 0;
	x = strtol(s, &e, 10);
	if (*e)
		return 0;
	*v = x;
	return 1;
}

static char *read_line(void)
{
	struct afh *a = fh_priv(me->pr_CIS);
	char *b = malloc(1024);
	int n = 0, c;

	if (!a) {
		b[0] = 0;
		return b;
	}
	while (n < 1022 && (c = fh_getc(a)) >= 0) {
		b[n++] = c;
		if (c == '\n')
			break;
	}
	b[n] = 0;
	return b;
}

static int is_help(const char *s, int32_t len)
{
	int32_t i = 0;

	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (i >= len || s[i] != '?')
		return 0;
	for (i++; i < len && s[i] != '\n'; i++)
		if (s[i] != ' ' && s[i] != '\t')
			return 0;
	return 1;
}

static void out_str(const char *s)
{
	struct afh *a = fh_priv(me->pr_COS);

	if (!a)
		return;
	while (*s)
		fh_putc(a, (unsigned char)*s++);
	fh_flush(a);
}

uint32_t ra_readargs(const char *tmpl, int32_t *array, struct RDArgs *rd)
{
	struct targ ta[64];
	char *res[64], **multi = NULL, *w, *line = NULL;
	int nmulti = 0, n, pos, last_pos, i, it, err = 0, start = 0;
	int32_t numv[64];
	struct src x;
	int filled[64];

	n = parse_template(tmpl, ta, 64);
	memset(res, 0, sizeof(res));
	memset(filled, 0, sizeof(filled));

	if (rd && rd->RDA_Source.CS_Buffer) {
		x.s = PTR(rd->RDA_Source.CS_Buffer);
		x.len = rd->RDA_Source.CS_Length;
		x.pos = rd->RDA_Source.CS_CurChr;
	} else {
		static char *copy;
		const char *s;
		int32_t l;
		if (dos_take_args(&s, &l)) {
			free(copy);
			copy = malloc(l + 1);
			memcpy(copy, s, l);
			x.s = copy;
			x.len = l;
		} else {
			x.s = dos_args;         /* a second ReadArgs: same line */
			x.len = dos_arglen;
		}
		x.pos = 0;
	}
	if (is_help(x.s + x.pos, x.len - x.pos) &&
	    !(rd && (rd->RDA_Flags & RDAF_NOPROMPT))) {
		for (;;) {
			out_str(tmpl);
			out_str(": ");
			free(line);
			line = read_line();
			if (rd && rd->RDA_ExtHelp && is_help(line, strlen(line))) {
				out_str(PTR(rd->RDA_ExtHelp));
				out_str("\n");
				continue;
			}
			break;
		}
		x.s = line;
		x.len = strlen(line);
		x.pos = 0;
	}

	multi = calloc(x.len + 2, sizeof(char *));
	/* pass 1 */
	pos = 0;
	while (pos <= n) {
		struct targ *t;
		int next;
		if (pos < n && filled[pos]) {
			pos++;
			continue;
		}
		if (pos < n && ta[pos].keyword) {
			pos++;
			continue;
		}
		next = pos + 1;
		t = pos < n ? &ta[pos] : NULL;
		start = x.pos;
		while (start < x.len && (x.s[start] == ' ' || x.s[start] == '\t'))
			start++;
		it = read_item(&x, &w);
		if (it == ITEM_ERROR) {
			free(w);
			err = ERROR_UNMATCHED_QUOTES;
			goto out;
		}
		if (it == ITEM_NOTHING) {
			free(w);
			break;
		}
		if (it == ITEM_EQUAL) {
			free(w);
			err = ERROR_BAD_TEMPLATE;
			goto out;
		}
		if (it == ITEM_UNQUOTED) {
			int k = find_key(ta, n, w);
			if ((k < 0 || filled[k]) && x.pos < x.len &&
			    x.s[x.pos] == '=') {
				/* not a keyword: "a=b" is one word */
				size_t l = strlen(w);
				while (x.pos < x.len && x.s[x.pos] != ' ' &&
				       x.s[x.pos] != '\t' && x.s[x.pos] != '\n')
					w[l++] = x.s[x.pos++];
				w[l] = 0;
			}
			if (k >= 0 && !filled[k]) {
				next = pos;
				pos = k;
				t = &ta[k];
				if (t->type != T_SWITCH && t->type != T_TOGGLE) {
					free(w);
					it = read_item(&x, &w);
					if (it == ITEM_EQUAL) {
						free(w);
						it = read_item(&x, &w);
					}
					if (it == ITEM_UNQUOTED) {
						/* where the value began */
						start = x.pos - strlen(w);
						if (x.pos <= x.len && x.pos > 0 &&
						    (x.s[x.pos - 1] == ' ' ||
						     x.s[x.pos - 1] == '\t'))
							start--;
					}
					if (it != ITEM_QUOTED &&
					    it != ITEM_UNQUOTED) {
						free(w);
						err = ERROR_KEY_NEEDS_ARG;
						goto out;
					}
				}
			}
		}
		if (t && t->type == T_FULL) {
			/* the rest of the line, as typed (a first word in
			   quotes loses them) */
			int32_t e = x.pos, from = x.pos;
			size_t l = strlen(w);
			char *full;
			while (e < x.len && x.s[e] != '\n')
				e++;
			if (it == ITEM_UNQUOTED) {
				from = start;
				l = 0;
			}
			full = malloc(l + (e - from) + 1);
			memcpy(full, w, l);
			memcpy(full + l, x.s + from, e - from);
			full[l + e - from] = 0;
			while (l + e - x.pos > 0 &&
			       (full[strlen(full) - 1] == ' ' ||
				full[strlen(full) - 1] == '\t'))
				full[strlen(full) - 1] = 0;
			free(w);
			x.pos = e;
			res[pos] = full;
			filled[pos] = 1;
			break;
		} else if (!t || t->multi) {
			multi[nmulti++] = w;
			next = pos;
		} else if (t->type == T_SWITCH || t->type == T_TOGGLE) {
			free(w);
			res[pos] = strdup("");
			filled[pos] = 1;
		} else {
			res[pos] = w;
			filled[pos] = 1;
		}
		pos = next;
	}
	last_pos = pos;

	/* pass 2: /A from the end of the multi list */
	for (i = n - 1; i >= 0; i--) {
		if (ta[i].required && !ta[i].multi && !filled[i]) {
			if (ta[i].keyword) {
				err = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}
			if (!nmulti) {
				err = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}
			res[i] = multi[--nmulti];
			multi[nmulti] = NULL;
			filled[i] = 1;
		}
	}
	for (i = 0; i < n; i++)
		if (ta[i].multi) {
			if (ta[i].required && !nmulti) {
				err = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}
			break;
		}
	if (i == n && nmulti && last_pos == n) {
		err = ERROR_TOO_MANY_ARGS;
		goto out;
	}
	/* pass 3: numbers */
	for (i = 0; i < n; i++)
		if (ta[i].type == T_NUM && filled[i] && !ta[i].multi &&
		    !parse_number(res[i], &numv[i])) {
			err = ERROR_BAD_NUMBER;
			goto out;
		}
	if (n && nmulti)
		for (i = 0; i < n; i++)
			if (ta[i].multi && ta[i].type == T_NUM)
				for (int k = 0; k < nmulti; k++) {
					int32_t v;
					if (!parse_number(multi[k], &v)) {
						err = ERROR_BAD_NUMBER;
						goto out;
					}
				}

	/* store */
	if (!rd) {
		rd = amem_alloc(sizeof(*rd));
		for (i = 0; i < 32; i++)
			if (!owned[i]) {
				owned[i] = rd;
				break;
			}
	}
	for (i = 0; i < n; i++) {
		struct targ *t = &ta[i];
		if (t->multi) {
			uint32_t *v;
			if (!nmulti) {
				array[i] = 0;
				continue;
			}
			v = da_alloc(rd, (nmulti + 1) * 4);
			for (int k = 0; k < nmulti; k++) {
				if (t->type == T_NUM) {
					int32_t *num = da_alloc(rd, 4);
					parse_number(multi[k], num);
					v[k] = APTR_OF(num);
				} else {
					char *s = da_alloc(rd, strlen(multi[k]));
					strcpy(s, multi[k]);
					v[k] = APTR_OF(s);
				}
			}
			v[nmulti] = 0;
			array[i] = APTR_OF(v);
			nmulti = 0;
			continue;
		}
		if (!filled[i])
			continue;
		switch (t->type) {
		case T_SWITCH:
			array[i] = DOSTRUE;
			break;
		case T_TOGGLE:
			array[i] = array[i] ? 0 : DOSTRUE;
			break;
		case T_NUM: {
			int32_t *num = da_alloc(rd, 4);
			*num = numv[i];
			array[i] = APTR_OF(num);
			break;
		}
		default: {
			char *s = da_alloc(rd, strlen(res[i]));
			strcpy(s, res[i]);
			array[i] = APTR_OF(s);
		}
		}
	}
	if (rd->RDA_Source.CS_Buffer)
		rd->RDA_Source.CS_CurChr = x.pos;
out:
	for (i = 0; i < 64; i++)
		free(res[i]);
	for (i = 0; multi && multi[i]; i++)
		free(multi[i]);
	free(multi);
	free(line);
	if (err) {
		dos_set_ioerr(err);
		return 0;
	}
	dos_set_ioerr(0);
	return APTR_OF(rd);
}

int32_t ra_readitem(char *buf, int32_t max, struct CSource *cs)
{
	struct src x;
	char *w;
	int it;

	if (cs && cs->CS_Buffer) {
		x.s = PTR(cs->CS_Buffer);
		x.len = cs->CS_Length;
		x.pos = cs->CS_CurChr;
		it = read_item(&x, &w);
		cs->CS_CurChr = x.pos;
	} else {
		/* from Input(): read the rest of a line, then put back what
		   the item did not use */
		struct afh *a = fh_priv(me->pr_CIS);
		char line[1024];
		int n = 0, c;
		while (a && n < 1023 && (c = fh_getc(a)) >= 0) {
			line[n++] = c;
			if (c == '\n')
				break;
		}
		x.s = line;
		x.len = n;
		x.pos = 0;
		it = read_item(&x, &w);
		if (a && x.pos < n) {
			/* push the unread part back into the buffer */
			int left = n - x.pos;
			if (a->rpos >= left) {
				a->rpos -= left;
				memcpy(a->rbuf + a->rpos, line + x.pos, left);
			}
		}
	}
	if (max > 0) {
		strncpy(buf, w, max - 1);
		buf[max - 1] = 0;
		if ((int32_t)strlen(w) >= max)
			it = ITEM_ERROR;
	}
	free(w);
	return it;
}
