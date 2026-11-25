/*
 * AmigaDOS wildcards:
 *   ?  any character        #x  zero or more x      (a|b)  alternatives
 *   ~x anything x does not match                    [a-z] [~a-z] classes
 *   %  the empty string     'x  x literally
 * '*' is a plain character, as on AmigaOS without the WILDSTAR flag
 * (set AMIGAFMT_WILDSTAR to make it mean #?).
 *
 * ParsePattern() writes the ROM's token format (dos/dosasl.h P_ANY etc.,
 * laid out as amitools' vamos PatternMatch.py does), because programs
 * build tokenized patterns by hand: C:Search hands MatchPatternNoCase
 * "P_ANY FOO P_ANY" it made itself.  The no-case variants upper-case the
 * pattern.  Matching turns the tokens into a tree and backtracks with
 * explicit continuations.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "af.h"

#define P_ANY      0x80
#define P_SINGLE   0x81
#define P_ORSTART  0x82
#define P_ORNEXT   0x83
#define P_OREND    0x84
#define P_NOT      0x85
#define P_NOTEND   0x86
#define P_NOTCLASS 0x87
#define P_CLASS    0x88
#define P_REPBEG   0x89
#define P_REPEND   0x8a
#define P_STOP     0x8b

enum { N_CHAR, N_ANY, N_CLASS, N_GROUP, N_REPEAT, N_NOT, N_STAR };

struct pnode;
struct pseq {
	struct pnode **v;
	int n;
};
struct pnode {
	int type;
	int c;
	char cls[256];
	struct pseq *alts;
	int nalts;
	struct pnode *child;
};

static int wildstar = -1;

static void init_star(void)
{
	if (wildstar < 0)
		wildstar = getenv("AMIGAFMT_WILDSTAR") != NULL;
}

/* ---- source -> tokens ---- */

static int tokenize(const char *src, uint8_t *dst, int32_t len, int nc,
		    int *wild)
{
	int32_t n = 0;
	uint8_t blocks[256];
	int nb = 0;
	const char *p = src;

#define PUT(c) do { if (n >= len - 1) return -1; dst[n++] = (c); } while (0)
#define PUSH(c) do { if (nb == 256) return -1; blocks[nb++] = (c); } while (0)
#define POP_NON_OR() while (nb && blocks[nb - 1] != P_OREND) PUT(blocks[--nb])
	*wild = 0;
	while (*p) {
		int flush = 1, c = (unsigned char)*p, next = (unsigned char)p[1];

		switch (c) {
		case '#':
			*wild = 1;
			if (!next || next == ')')
				return -1;
			if (next == '?') {
				PUT(P_ANY);
				p++;
			} else {
				PUT(P_REPBEG);
				PUSH(P_REPEND);
				flush = 0;
			}
			break;
		case '~':
			if (!next) {
				PUT('~');
			} else if (next == ')') {
				return -1;
			} else {
				*wild = 1;
				PUT(P_NOT);
				PUSH(P_NOTEND);
				flush = 0;
			}
			break;
		case '?':
			*wild = 1;
			PUT(P_SINGLE);
			break;
		case '(':
			*wild = 1;
			PUT(P_ORSTART);
			PUSH(P_OREND);
			flush = 0;
			break;
		case '|':
			POP_NON_OR();
			if (!nb)
				return -1;
			PUT(P_ORNEXT);
			flush = 0;
			break;
		case ')':
			POP_NON_OR();
			if (!nb)
				return -1;
			PUT(P_OREND);
			nb--;
			break;
		case '[':
			*wild = 1;
			if (next == '~') {
				PUT(P_NOTCLASS);
				p += 2;
			} else {
				PUT(P_CLASS);
				p++;
			}
			for (; *p && *p != ']'; p++) {
				int k = (unsigned char)*p;
				if (k == '\'' && p[1])
					k = (unsigned char)*++p;
				PUT(nc ? toupper(k) : k);
			}
			if (!*p)
				return -1;
			PUT(P_CLASS);
			break;
		case '*':
			if (wildstar) {
				*wild = 1;
				PUT(P_ANY);
			} else
				PUT('*');
			break;
		case '%':
			flush = 0;
			break;
		case '\'':
			if (next && strchr("#*?(|)~[]%'", next)) {
				PUT(next);
				p++;
			}
			break;
		default:
			PUT(nc ? toupper(c) : c);
		}
		p++;
		if (flush)
			POP_NON_OR();
	}
	if (nb)
		return -1;
	dst[n] = 0;
	return n;
#undef PUT
#undef PUSH
#undef POP_NON_OR
}

/* ---- tokens -> tree ---- */

static struct pnode *newnode(int t)
{
	struct pnode *n = calloc(1, sizeof(*n));
	n->type = t;
	return n;
}

static void seq_add(struct pseq *s, struct pnode *n)
{
	s->v = realloc(s->v, (s->n + 1) * sizeof(*s->v));
	s->v[s->n++] = n;
}

static void tok_seq(const uint8_t **pp, struct pseq *seq, int *err,
		    int t1, int t2);

static struct pnode *tok_group(const uint8_t **pp, int *err, int end)
{
	struct pnode *g = newnode(N_GROUP);

	g->alts = calloc(1, sizeof(struct pseq));
	g->nalts = 1;
	for (;;) {
		tok_seq(pp, &g->alts[g->nalts - 1], err, P_ORNEXT, end);
		if (*err || !**pp) {
			*err = 1;
			return g;
		}
		if (**pp == end) {
			(*pp)++;
			return g;
		}
		(*pp)++;                        /* P_ORNEXT */
		g->alts = realloc(g->alts, (g->nalts + 1) * sizeof(struct pseq));
		memset(&g->alts[g->nalts++], 0, sizeof(struct pseq));
	}
}

static void tok_seq(const uint8_t **pp, struct pseq *seq, int *err,
		    int t1, int t2)
{
	while (**pp && **pp != t1 && **pp != t2 && **pp != P_STOP && !*err) {
		int c = *(*pp)++;
		struct pnode *n;

		switch (c) {
		case P_ANY:
			n = newnode(N_STAR);
			break;
		case P_SINGLE:
			n = newnode(N_ANY);
			break;
		case P_REPBEG:
			n = newnode(N_REPEAT);
			n->child = tok_group(pp, err, P_REPEND);
			break;
		case P_NOT:
			n = newnode(N_NOT);
			n->child = tok_group(pp, err, P_NOTEND);
			break;
		case P_ORSTART:
			n = tok_group(pp, err, P_OREND);
			break;
		case P_CLASS:
		case P_NOTCLASS:
			n = newnode(N_CLASS);
			while (**pp && **pp != P_CLASS) {
				int lo = *(*pp)++, hi = lo;
				if (**pp == '-') {
					(*pp)++;
					if (**pp == P_CLASS || !**pp)
						hi = 255;
					else
						hi = *(*pp)++;
				}
				for (int k = lo; k <= hi; k++)
					n->cls[k] = 1;
			}
			if (**pp == P_CLASS)
				(*pp)++;
			else
				*err = 1;
			if (c == P_NOTCLASS)
				for (int k = 0; k < 256; k++)
					n->cls[k] = !n->cls[k];
			break;
		default:
			n = newnode(N_CHAR);
			n->c = c;
		}
		seq_add(seq, n);
	}
}

static void free_node(struct pnode *n);

static void free_seq(struct pseq *s)
{
	for (int i = 0; i < s->n; i++)
		free_node(s->v[i]);
	free(s->v);
}

static void free_node(struct pnode *n)
{
	if (!n)
		return;
	for (int i = 0; i < n->nalts; i++)
		free_seq(&n->alts[i]);
	free(n->alts);
	free_node(n->child);
	free(n);
}

/* ---- matching ---- */

struct cont {
	struct pseq *seq;
	int i;
	char *record;           /* non-NULL: record end positions, fail */
	const char *rbase;
	struct cont *next;
};

static int nocase;
static const char *subject_end;

static int ceq(int a, int b)
{
	if (nocase)
		return toupper(a) == toupper(b);
	return a == b;
}

static int mseq(struct pseq *s, int i, const char *str, struct cont *k);

static int run_cont(const char *str, struct cont *k)
{
	if (!k)
		return str == subject_end;
	if (k->record) {
		k->record[str - k->rbase] = 1;
		return 0;
	}
	return mseq(k->seq, k->i, str, k->next);
}

/* every length the node n can match at str, into ends[] */
static void ends_of(struct pnode *n, const char *str, char *ends)
{
	struct pseq one = { &n, 1 };
	struct cont rec = { NULL, 0, ends, str, NULL };

	mseq(&one, 0, str, &rec);
}

static int mseq(struct pseq *s, int i, const char *str, struct cont *k)
{
	struct pnode *n;

	if (i == s->n)
		return run_cont(str, k);
	n = s->v[i];
	switch (n->type) {
	case N_CHAR:
		return str < subject_end && ceq((unsigned char)*str, n->c) &&
		       mseq(s, i + 1, str + 1, k);
	case N_ANY:
		return str < subject_end && mseq(s, i + 1, str + 1, k);
	case N_CLASS: {
		int c = (unsigned char)*str;
		if (str >= subject_end)
			return 0;
		if (!n->cls[c] && !(nocase && (n->cls[toupper(c)] ||
					       n->cls[tolower(c)])))
			return 0;
		return mseq(s, i + 1, str + 1, k);
	}
	case N_STAR:
		for (const char *e = str; e <= subject_end; e++)
			if (mseq(s, i + 1, e, k))
				return 1;
		return 0;
	case N_GROUP: {
		struct cont c = { s, i + 1, NULL, NULL, k };
		for (int a = 0; a < n->nalts; a++)
			if (mseq(&n->alts[a], 0, str, &c))
				return 1;
		return 0;
	}
	case N_REPEAT: {
		size_t len = subject_end - str;
		char *ends;
		int r = 0;
		if (mseq(s, i + 1, str, k))
			return 1;
		if (!len)
			return 0;
		ends = calloc(len + 1, 1);
		ends_of(n->child, str, ends);
		for (size_t e = len; e >= 1 && !r; e--)
			if (ends[e])
				r = mseq(s, i, str + e, k);     /* repeat again */
		free(ends);
		return r;
	}
	case N_NOT: {
		size_t len = subject_end - str;
		char *ends = calloc(len + 1, 1);
		int r = 0;
		ends_of(n->child, str, ends);
		for (size_t e = len + 1; e-- > 0 && !r; )
			if (!ends[e])
				r = mseq(s, i + 1, str + e, k);
		free(ends);
		return r;
	}
	}
	return 0;
}

static int match_tokens(const uint8_t *tok, const char *str, int nc)
{
	struct pseq top = { NULL, 0 };
	const uint8_t *p = tok;
	int err = 0, r;

	tok_seq(&p, &top, &err, -1, -1);
	nocase = nc;
	subject_end = str + strlen(str);
	r = !err && mseq(&top, 0, str, NULL);
	free_seq(&top);
	return r;
}

/* ---- the dos.library side ---- */

int32_t pat_parse(const char *src, char *dst, int32_t len, int nc)
{
	int wild;

	init_star();
	if (tokenize(src, (uint8_t *)dst, len, nc, &wild) < 0)
		return -1;
	return wild;
}

int pat_match(const char *pat, const char *str, int nc)
{
	return match_tokens((const uint8_t *)pat, str, nc);
}

/* match a pattern that has not been through ParsePattern */
int pat_matchraw(const char *pat, const char *str, int nc)
{
	uint8_t buf[1024];
	int wild;

	init_star();
	if (tokenize(pat, buf, sizeof(buf), nc, &wild) < 0)
		return 0;
	return match_tokens(buf, str, nc);
}
