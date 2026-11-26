/*
 * The shell: command lines, scripts and the shell's own commands.
 *
 * A command line is parsed as the AmigaOS 3.1 shell does it: `;' starts a
 * comment, $name and ${name} are replaced by local then global variables
 * (`*$' is a literal dollar), `command` by that command's output, an
 * alias in the first word by its value ([] standing for the rest of the
 * line), redirections (<file, >file, >>file) follow the command name, and
 * the rest is the command's argument line.  The command is looked up in
 * the resident list (which holds the shell's own commands), then as a
 * path, in the current directory, along the path (cli_CommandDir) and in
 * C:.  A name found nowhere is handed to the Linux shell (/bin/sh -c),
 * which reports it when it is no Linux command either.
 *
 * Scripts are what C:Execute leaves behind: it writes the script, with
 * its .key arguments substituted, to a T: file and makes that the CLI's
 * cli_CurrentInput (cli_CommandFile names it).  Whenever a command
 * returns with the current input changed, the shell reads commands from
 * it until it ends, then deletes the file and goes back to the standard
 * input.  A command whose return code reaches the fail level ends the
 * script.  amigafmt -c gives the shell lines the same way.
 *
 * The shell's own commands (Echo, If, Set, CD, Path, ...) are C here.  Each
 * is also on the resident list with a seglist that enters it through a
 * hidden library's vector, so FindSegment() finds them as INTERNAL and
 * RunCommand() runs them like any program.
 *
 * Everything runs in this process: System() and Execute() run a nested
 * shell with the CLI's state saved around it.  Run forks.
 */
#include <dirent.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "dos_int.h"

#define CMD_SYSTEM    (-1)
#define CMD_INTERNAL  (-2)
#define LV_VAR   0
#define LV_ALIAS 1

struct Segment {
	BPTR seg_Next;
	int32_t seg_UC;
	BPTR seg_Seg;
	uint8_t seg_Name[4];            /* a BSTR, as long as it needs */
};

struct LocalVar {
	struct Node lv_Node;
	uint16_t lv_Flags;
	APTR lv_Value;
	uint32_t lv_Len;
};

/* the state of the running script: IF nesting and SKIP */
struct shstate {
	int skip_if;            /* 1: skipping an IF's false branch, 2: an
				   ELSE branch after a true IF */
	int skip_depth;         /* IFs opened inside the skipped lines */
	int skip_label;         /* SKIP in progress */
	char label[64];
	int quit;               /* QUIT: the script ends */
	int32_t quit_rc;
	int endshell;           /* EndShell/EndCLI: the shell ends */
};

static struct shstate sh;
static BPTR seg_head;           /* the resident list */

/* ---- small helpers ---- */

/* the terminal, for output when there is no other */
static BPTR console_out(void)
{
	static BPTR fh;

	if (!fh)
		fh = fh_new(-1, 1, 1, NULL);
	return fh;
}

static struct CommandLineInterface *cli(void)
{
	return dos_cli();
}

static void bset(BPTR b, const char *s)
{
	uint8_t *d = BADDR(b);
	size_t n = strlen(s);

	if (!b)
		return;
	if (n > 255)
		n = 255;
	d[0] = n;
	memcpy(d + 1, s, n);
}

static void bget(BPTR b, char *buf, size_t len)
{
	uint8_t *s = BADDR(b);
	size_t n = b ? s[0] : 0;

	if (n >= len)
		n = len - 1;
	if (n)
		memcpy(buf, s + 1, n);
	buf[n] = 0;
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
	struct afh *a = fh_priv(me->pr_COS);
	char buf[2048];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (a && n > 0)
		fh_write(a, buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

/* an error message, as PrintFault() writes it */
static void fault(int32_t code, const char *hdr)
{
	char buf[300];

	dos_fault_text(code, hdr, buf, sizeof(buf));
	out("%s\n", buf);
	dos_set_ioerr(code);
}

static void set_rc(int32_t rc, int32_t r2)
{
	char n[16];

	cli()->cli_ReturnCode = rc;
	cli()->cli_Result2 = r2;
	snprintf(n, sizeof(n), "%d", (int)rc);
	var_set_local("RC", n, -1, LV_VAR);
	snprintf(n, sizeof(n), "%d", (int)r2);
	var_set_local("Result2", n, -1, LV_VAR);
}

/* ReadArgs() over a string: the shell's own commands parse with this */
static struct RDArgs *rdargs(const char *tmpl, int32_t *array, int n,
			     const char *args)
{
	struct RDArgs *rd = amem_alloc(sizeof(*rd));
	size_t l = strlen(args);
	char *copy = amem_alloc(l + 2);

	memcpy(copy, args, l);
	if (!l || copy[l - 1] != '\n')
		copy[l++] = '\n';
	copy[l] = 0;
	memset(array, 0, n * sizeof(int32_t));
	rd->RDA_Source.CS_Buffer = APTR_OF(copy);
	rd->RDA_Source.CS_Length = l;
	rd->RDA_Flags = RDAF_NOPROMPT;
	if (!ra_readargs(tmpl, array, rd)) {
		amem_free(copy);
		amem_free(rd);
		return NULL;
	}
	return rd;
}

static void rdfree(struct RDArgs *rd)
{
	if (!rd)
		return;
	amem_free(PTR(rd->RDA_Source.CS_Buffer));
	ra_freeargs(rd);
	amem_free(rd);
}

#define ARGS(tmpl, n) \
	int32_t arg[n]; \
	struct RDArgs *rd = rdargs(tmpl, arg, n, args); \
	if (!rd) { \
		fault(me->pr_Result2 ? me->pr_Result2 : ERROR_REQUIRED_ARG_MISSING, NULL); \
		return 10; \
	}


/* ---- the resident list ---- */

static struct Segment *seg_new(const char *name, BPTR seglist, int32_t uc)
{
	size_t n = strlen(name);
	struct Segment *s = amem_alloc(sizeof(*s) + n + 4);

	s->seg_UC = uc;
	s->seg_Seg = seglist;
	s->seg_Name[0] = n;
	memcpy(s->seg_Name + 1, name, n);
	return s;
}

static int seg_named(struct Segment *s, const char *name)
{
	return s->seg_Name[0] == strlen(name) &&
	       !strncasecmp((char *)s->seg_Name + 1, name, s->seg_Name[0]);
}

struct Segment *shell_find_segment(const char *name, struct Segment *start,
				   int system)
{
	struct Segment *s = start ? BADDR(start->seg_Next) : BADDR(seg_head);

	for (; s; s = BADDR(s->seg_Next))
		if (seg_named(s, name) && (system || s->seg_UC >= 0) &&
		    s->seg_UC != -999)
			return s;
	return NULL;
}

static void d_FindSegment(struct frame *f)
{
	RET(shell_find_segment(PTR(D(1)), PTR(D(2)), D(3)));
}

static void seg_push(struct Segment *s)
{
	s->seg_Next = seg_head;
	seg_head = MKBADDR(s);
	dos_set_reslist(seg_head);
}

static void d_AddSegment(struct frame *f)
{
	seg_push(seg_new(PTR(D(1)), D(2), D(3)));
	RET(DOSTRUE);
}

static int seg_remove(struct Segment *r)
{
	BPTR *p;

	if (r->seg_UC != 0)
		return 0;
	for (p = &seg_head; *p; p = &((struct Segment *)BADDR(*p))->seg_Next)
		if (BADDR(*p) == r) {
			*p = r->seg_Next;
			dos_set_reslist(seg_head);
			hunk_unload(r->seg_Seg);
			amem_free(r);
			return 1;
		}
	return 0;
}

static void d_RemSegment(struct frame *f)
{
	RET(seg_remove(PTR(D(1))) ? DOSTRUE : DOSFALSE);
}

/* ---- running a command ---- */

static int32_t run_script_input(void);

/* the Linux file of a command, or NULL: a path, the current directory,
   the path list, C: */
static char *find_command(const char *name)
{
	struct stat st;
	char *lp;
	BPTR p;

	if (strchr(name, ':') || strchr(name, '/')) {
		lp = path_to_linux(name, NULL);
		if (lp && !stat(lp, &st) && S_ISREG(st.st_mode))
			return lp;
		free(lp);
		return NULL;
	}
	lp = path_to_linux(name, NULL);
	if (lp && !stat(lp, &st) && S_ISREG(st.st_mode))
		return lp;
	free(lp);
	for (p = cli()->cli_CommandDir; p; p = ((BPTR *)BADDR(p))[0]) {
		lp = path_to_linux(name, lock_path(((BPTR *)BADDR(p))[1]));
		if (lp && !stat(lp, &st) && S_ISREG(st.st_mode))
			return lp;
		free(lp);
	}
	{
		char a[300];
		snprintf(a, sizeof(a), "C:%s", name);
		lp = path_to_linux(a, NULL);
	}
	if (lp && !stat(lp, &st) && S_ISREG(st.st_mode))
		return lp;
	free(lp);
	return NULL;
}

static int32_t (*internal_fn(struct Segment *s))(const char *);

/*
 * Run one command: name, redirections and argument line already split.
 * Returns its return code, or -1 when it could not be run at all.
 */
static int32_t run_command(const char *name, const char *args,
			   const char *rin, const char *rout, int append,
			   const char *whole)
{
	struct CommandLineInterface *c = cli();
	struct Segment *res;
	int32_t (*fn)(const char *) = NULL;
	BPTR seg = 0, oin = me->pr_CIS, oout = me->pr_COS, fin = 0, fout = 0;
	BPTR odir = me->pr_CurrentDir, ohome = me->pr_HomeDir, home = 0;
	char oldname[256], *lp = NULL, *oprogdir = NULL;
	int err, loaded = 0;
	int32_t rc = -1;

	res = shell_find_segment(name, NULL, 1);
	if (res) {
		fn = internal_fn(res);
		if (!fn) {
			seg = res->seg_Seg;
			if (res->seg_UC >= 0)
				res->seg_UC++;
		}
	} else if ((lp = find_command(name))) {
		seg = hunk_load(lp, &err);
		if (!seg) {
			free(lp);
			fault(err ? err : ERROR_FILE_NOT_OBJECT, name);
			return 10;
		}
		loaded = 1;
	} else if (strchr(name, ':')) {
		out("%s: Unknown command\n", name);
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
		return 10;
	} else {
		/* not an AmigaOS command: the Linux shell, which says so
		   itself when it is not one either */
		int st;
		dos_flush_all();
		st = system(whole);
		if (st < 0 || !WIFEXITED(st))
			return -1;
		if (WEXITSTATUS(st) == 127) {
			dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
			return 10;
		}
		return WEXITSTATUS(st);
	}

	if (rin && !(fin = dos_open(rin, MODE_OLDFILE))) {
		fault(me->pr_Result2, rin);
		goto out;
	}
	if (rout && !(fout = dos_open(rout, append ? MODE_READWRITE :
				      MODE_NEWFILE))) {
		fault(me->pr_Result2, rout);
		goto out;
	}
	if (fout && append)
		lseek(fh_priv(fout)->wfd, 0, SEEK_END);
	if (fin)
		me->pr_CIS = fin;
	if (fout)
		me->pr_COS = fout;
	bget(c->cli_CommandName, oldname, sizeof(oldname));
	bset(c->cli_CommandName, name);
	if (fn) {
		rc = fn(args);
	} else {
		/* PROGDIR: is the command's own directory while it runs */
		if (lp) {
			char *s = strrchr(lp, '/');
			oprogdir = path_to_linux("PROGDIR:", "/");
			if (s) {
				*s = 0;
				path_assign("PROGDIR", *lp ? lp : "/");
				home = lock_new(*lp ? lp : "/", SHARED_LOCK);
				me->pr_HomeDir = home;
				*s = '/';
			}
		}
		c->cli_Module = seg;
		rc = dos_runcommand(seg, c->cli_DefaultStack * 4, args,
				    strlen(args));
		c->cli_Module = 0;
		me->pr_HomeDir = ohome;
		if (oprogdir)
			path_assign("PROGDIR", oprogdir);
		if (home)
			lock_free(home);
		if (me->pr_CurrentDir != odir) {        /* its CD is its own */
			me->pr_CurrentDir = odir;
			path_set_cwd(lock_path(odir));
		}
	}
	bset(c->cli_CommandName, oldname);
	dos_flush_all();
out:
	me->pr_CIS = oin;
	me->pr_COS = oout;
	if (fin)
		dos_close(fin);
	if (fout)
		dos_close(fout);
	if (res && !fn && res->seg_UC > 0)
		res->seg_UC--;
	if (loaded)
		hunk_unload(seg);
	free(lp);
	free(oprogdir);
	return rc;
}

/* ---- parsing ---- */

struct sbuf {
	char *s;
	size_t n, cap;
};

static void sb_add(struct sbuf *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2 + 64;
		b->s = realloc(b->s, b->cap);
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}

static void sb_addc(struct sbuf *b, char c) { sb_add(b, &c, 1); }

static char *backtick(const char *cmd);

/* a variable's value for $name: local, then global */
static char *var_value(const char *name)
{
	struct LocalVar *v = var_find(name, LV_VAR);
	int32_t len;
	char *g;

	if (v) {
		g = malloc(v->lv_Len + 1);
		memcpy(g, PTR(v->lv_Value), v->lv_Len);
		g[v->lv_Len] = 0;
		return g;
	}
	if ((g = var_get_global(name, &len))) {
		char *nl = memchr(g, '\n', len);
		g[nl ? nl - g : len] = 0;
	}
	return g;
}

/*
 * Comments, variables and backticks: what the shell does to a line before
 * it looks at the words.
 */
static char *expand(const char *line)
{
	struct sbuf b = { 0 };
	int q = 0;
	const char *p;

	sb_add(&b, "", 0);
	for (p = line; *p; p++) {
		if (*p == '"')
			q = !q;
		if (!q && *p == ';')
			break;
		if (*p == '*' && p[1] == '$') {         /* a literal $ */
			sb_addc(&b, '$');
			p++;
			continue;
		}
		if (*p == '*' && q && p[1]) {           /* *" and ** stay */
			sb_add(&b, p, 2);
			p++;
			continue;
		}
		if (*p == '$' && (p[1] == '{' || p[1] == '_' ||
				  (p[1] >= 'a' && p[1] <= 'z') ||
				  (p[1] >= 'A' && p[1] <= 'Z') ||
				  (p[1] >= '0' && p[1] <= '9'))) {
			char name[256];
			const char *s = p + 1;
			size_t n = 0;
			char *v;
			if (*s == '{') {
				const char *e = strchr(s, '}');
				if (!e)
					goto literal;
				n = e - s - 1;
				if (n >= sizeof(name))
					n = sizeof(name) - 1;
				memcpy(name, s + 1, n);
				s = e + 1;
			} else {
				while (n < sizeof(name) - 1 &&
				       (*s == '_' || (*s >= 'a' && *s <= 'z') ||
					(*s >= 'A' && *s <= 'Z') ||
					(*s >= '0' && *s <= '9')))
					name[n++] = *s++;
			}
			name[n] = 0;
			v = var_value(name);
			if (!v)
				goto literal;   /* an unset variable stays as it is */
			sb_add(&b, v, strlen(v));
			free(v);
			p = s - 1;
			continue;
		}
		if (*p == '`' && !q) {
			const char *e = strchr(p + 1, '`');
			char *cmd, *res;
			if (!e)
				goto literal;
			cmd = strndup(p + 1, e - p - 1);
			res = backtick(cmd);
			sb_add(&b, res, strlen(res));
			free(res);
			free(cmd);
			p = e;
			continue;
		}
literal:
		sb_addc(&b, *p);
	}
	while (b.n && (b.s[b.n - 1] == ' ' || b.s[b.n - 1] == '\t' ||
		       b.s[b.n - 1] == '\n' || b.s[b.n - 1] == '\r'))
		b.s[--b.n] = 0;
	return b.s;
}

/* the first word of a line (quotes removed) and where the rest starts */
static const char *first_word(const char *p, char *w, size_t len)
{
	size_t n = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '"') {
		for (p++; *p && *p != '"'; p++)
			if (n < len - 1)
				w[n++] = *p;
		if (*p == '"')
			p++;
	} else
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			if (n < len - 1)
				w[n++] = *p++;
			else
				p++;
	w[n] = 0;
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* one expanded line: alias, redirections, then run it */
static int32_t run_line_expanded(const char *line)
{
	char name[256], tok[512], *rin = NULL, *rout = NULL, *args;
	const char *p;
	struct LocalVar *al;
	char *aliased = NULL;
	int append = 0;
	size_t n;
	int32_t rc;

	p = first_word(line, name, sizeof(name));
	if (!*name)
		return 0;
	if ((al = var_find(name, LV_ALIAS))) {
		/* the alias's value, with [] standing for the rest */
		struct sbuf b = { 0 };
		const char *v = PTR(al->lv_Value), *br;
		size_t vl = al->lv_Len;
		char *val = strndup(v, vl);
		sb_add(&b, "", 0);
		if ((br = strstr(val, "[]"))) {
			sb_add(&b, val, br - val);
			sb_add(&b, p, strlen(p));
			sb_add(&b, br + 2, strlen(br + 2));
		} else {
			sb_add(&b, val, strlen(val));
			if (*p) {
				sb_addc(&b, ' ');
				sb_add(&b, p, strlen(p));
			}
		}
		free(val);
		aliased = b.s;
		p = first_word(aliased, name, sizeof(name));
		if (!*name) {
			free(aliased);
			return 0;
		}
	}
	for (;;) {
		int isin = *p == '<', isout = *p == '>';
		size_t k = 0;
		if (!isin && !isout)
			break;
		p++;
		if (isout && *p == '>')
			append = 1, p++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++)
				if (k < sizeof(tok) - 1)
					tok[k++] = *p;
			if (*p)
				p++;
		} else
			while (*p && *p != ' ' && *p != '\t' && k < sizeof(tok) - 1)
				tok[k++] = *p++;
		tok[k] = 0;
		if (isin) {
			free(rin);
			rin = strdup(tok);
		} else {
			free(rout);
			rout = strdup(tok);
		}
		while (*p == ' ' || *p == '\t')
			p++;
	}
	n = strlen(p);
	args = malloc(n + 2);
	memcpy(args, p, n);
	args[n++] = '\n';
	args[n] = 0;
	rc = run_command(name, args, rin, rout, append, aliased ? aliased : line);
	free(args);
	free(rin);
	free(rout);
	free(aliased);
	return rc;
}

/* run a line, with the return code bookkeeping the shell does */
static int32_t shell_line(const char *raw)
{
	char *line = expand(raw);
	int32_t rc;

	if (!*line) {
		free(line);
		return 0;
	}
	dos_set_ioerr(0);
	rc = run_line_expanded(line);
	set_rc(rc < 0 ? 20 : rc, me->pr_Result2);
	free(line);
	return rc;
}

/* a temporary file in $TMPDIR (or /tmp), its name in *path to free */
static int shell_mkstemp(char **path)
{
	const char *dir = getenv("TMPDIR");
	size_t n;
	int fd;

	if (!dir || !*dir)
		dir = "/tmp";
	n = strlen(dir) + sizeof("/afshell.XXXXXX");
	*path = malloc(n);
	if (!*path)
		return -1;
	snprintf(*path, n, "%s/afshell.XXXXXX", dir);
	fd = mkstemp(*path);
	if (fd < 0) {
		free(*path);
		*path = NULL;
	}
	return fd;
}

/* `command`: what it writes, newlines turned into spaces */
static char *backtick(const char *cmd)
{
	char *tmpl;
	int fd = shell_mkstemp(&tmpl);
	BPTR fh, oout = me->pr_COS;
	struct sbuf b = { 0 };
	char buf[512];
	ssize_t n;

	sb_add(&b, "", 0);
	if (fd < 0)
		return b.s;
	fh = fh_new(fd, fd, 0, tmpl);
	me->pr_COS = fh;
	shell_line(cmd);
	dos_flush_all();
	me->pr_COS = oout;
	lseek(fd, 0, SEEK_SET);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		sb_add(&b, buf, n);
	dos_close(fh);
	unlink(tmpl);
	free(tmpl);
	for (size_t i = 0; i < b.n; i++)
		if (b.s[i] == '\n')
			b.s[i] = ' ';
	while (b.n && b.s[b.n - 1] == ' ')
		b.s[--b.n] = 0;
	return b.s;
}

/* ---- scripts ---- */

static char *read_line(BPTR fh)
{
	struct afh *a = fh_priv(fh);
	struct sbuf b = { 0 };
	int c, any = 0;

	if (!a)
		return NULL;
	sb_add(&b, "", 0);
	while ((c = fh_getc(a)) >= 0) {
		any = 1;
		if (c == '\n')
			break;
		sb_addc(&b, c);
	}
	if (!any) {
		free(b.s);
		return NULL;
	}
	return b.s;
}

/* back to the standard input: the script is over */
static void end_script(void)
{
	struct CommandLineInterface *c = cli();
	char name[256];

	if (c->cli_CurrentInput != c->cli_StandardInput) {
		dos_close(c->cli_CurrentInput);
		c->cli_CurrentInput = c->cli_StandardInput;
	}
	bget(c->cli_CommandFile, name, sizeof(name));
	if (*name) {
		char *lp = path_to_linux(name, NULL);
		if (lp)
			unlink(lp);
		free(lp);
		bset(c->cli_CommandFile, "");
	}
	c->cli_FailLevel = 10;
	sh.skip_if = sh.skip_depth = sh.skip_label = 0;
}

/* a line read while skipping: does it end the skip? */
static void skip_line(const char *line)
{
	char w[64], arg[64];
	const char *p = first_word(line, w, sizeof(w));

	if (sh.skip_label) {
		if (!strcasecmp(w, "Lab")) {
			first_word(p, arg, sizeof(arg));
			if (!sh.label[0] || !strcasecmp(arg, sh.label))
				sh.skip_label = 0;
		} else if (!strcasecmp(w, "EndSkip") && !sh.label[0])
			sh.skip_label = 0;
		return;
	}
	if (!strcasecmp(w, "If"))
		sh.skip_depth++;
	else if (!strcasecmp(w, "EndIf")) {
		if (sh.skip_depth)
			sh.skip_depth--;
		else
			sh.skip_if = 0;
	} else if (!strcasecmp(w, "Else") && !sh.skip_depth && sh.skip_if == 1)
		sh.skip_if = 0;
}

/*
 * Read and run commands from cli_CurrentInput until it is the standard
 * input again (the script has ended), or, interactively, until the
 * standard input ends.  Returns the last return code.
 */
static int32_t shell_loop(int interactive)
{
	struct CommandLineInterface *c = cli();
	int32_t rc = c->cli_ReturnCode;

	for (;;) {
		BPTR in = c->cli_CurrentInput;
		int script = in != c->cli_StandardInput;
		char *line, word[256];

		if (!script && !interactive)
			break;
		if (!script) {
			char pr[256], o[512];
			size_t k = 0;
			bget(c->cli_Prompt, pr, sizeof(pr));
			for (char *s = pr; *s && k < sizeof(o) - 64; s++) {
				if (*s != '%' || !s[1]) {
					o[k++] = *s;
					continue;
				}
				s++;
				if (*s == 'N' || *s == 'n')
					k += snprintf(o + k, sizeof(o) - k, "%d",
						      (int)me->pr_TaskNum);
				else if (*s == 'S' || *s == 's') {
					char *d = path_to_amiga(path_cwd());
					k += snprintf(o + k, sizeof(o) - k, "%s", d);
					free(d);
				} else if (*s == 'R' || *s == 'r')
					k += snprintf(o + k, sizeof(o) - k, "%d",
						      (int)c->cli_ReturnCode);
				else
					o[k++] = *s;
			}
			o[k] = 0;
			out("%s", o);
			dos_flush_all();
		}
		line = read_line(in);
		if (!line) {
			if (script) {
				end_script();
				continue;
			}
			break;
		}
		if (sh.skip_if || sh.skip_label) {
			skip_line(line);
			free(line);
			continue;
		}
		first_word(line, word, sizeof(word));
		/* a command's streams are the shell's, whatever the last one
		   selected (C:Execute leaves its script as Input()) */
		me->pr_CIS = c->cli_StandardInput;
		me->pr_COS = c->cli_StandardOutput;
		rc = shell_line(line);
		free(line);
		if (rc < 0)
			rc = 20;
		if (sh.endshell) {
			sh.endshell = 0;
			while (c->cli_CurrentInput != c->cli_StandardInput)
				end_script();
			if (interactive)
				out("Process %d ending\n", (int)me->pr_TaskNum);
			break;
		}
		if (sh.quit) {
			sh.quit = 0;
			rc = sh.quit_rc;
			set_rc(rc, 0);
			if (c->cli_CurrentInput != c->cli_StandardInput)
				end_script();
			continue;
		}
		if (c->cli_CurrentInput != c->cli_StandardInput &&
		    rc >= c->cli_FailLevel) {
			out("%s failed returncode %d\n", word, (int)rc);
			end_script();
		}
		if (sh.skip_label && c->cli_CurrentInput == c->cli_StandardInput)
			sh.skip_label = 0;
	}
	return rc;
}

static int32_t run_script_input(void)
{
	return shell_loop(0);
}

/* after a command run directly: a script it started (C:Execute) */
int32_t shell_after(int32_t rc)
{
	struct CommandLineInterface *c = cli();

	if (!c || c->cli_CurrentInput == c->cli_StandardInput)
		return rc;
	set_rc(rc, me->pr_Result2);
	return run_script_input();
}

/*
 * System() and Execute(): the line, and any script it starts, in a nested
 * shell; the CLI's own script state is saved around it.  in is the
 * commands' input (System()'s SYS_Input), or with commands set, where
 * further commands are read from (Execute()).
 */
int32_t shell_system(const char *line, BPTR in, BPTR out_fh, int commands)
{
	struct CommandLineInterface *c = cli();
	struct shstate saved = sh;
	BPTR cur = c->cli_CurrentInput, std = c->cli_StandardInput;
	BPTR stdout_ = c->cli_StandardOutput, curout = c->cli_CurrentOutput;
	BPTR oin = me->pr_CIS, oout = me->pr_COS;
	BPTR file = c->cli_CommandFile;
	BPTR odir = me->pr_CurrentDir;
	int32_t fail = c->cli_FailLevel, rc;
	uint8_t *nf = amem_alloc(257);

	memset(&sh, 0, sizeof(sh));
	/* the nested shell's current directory is its own copy, as a new
	   shell process's would be: its CD unlocks that, not the caller's */
	me->pr_CurrentDir = lock_new(lock_path(odir), SHARED_LOCK);
	c->cli_CommandFile = MKBADDR(nf);
	if (out_fh)
		me->pr_COS = out_fh;
	else if (in && fh_priv(in) && fh_priv(in)->wfd >= 0)
		me->pr_COS = in;        /* output to the window given as input */
	if (!me->pr_COS)                /* a Workbench process has none */
		me->pr_COS = console_out();
	if (in && !commands)
		me->pr_CIS = in;
	c->cli_StandardInput = c->cli_CurrentInput = me->pr_CIS;
	c->cli_StandardOutput = c->cli_CurrentOutput = me->pr_COS;
	/* a command string may hold several lines (IconX gives two) */
	rc = 0;
	for (const char *p = line; *p; ) {
		size_t n = strcspn(p, "\n");
		char *one = strndup(p, n);
		rc = shell_line(one);
		free(one);
		if (c->cli_CurrentInput != c->cli_StandardInput)
			rc = run_script_input();
		p += n + (p[n] == '\n');
	}
	if (in && commands) {
		c->cli_CurrentInput = in;
		rc = run_script_input();
	}
	c->cli_CurrentInput = cur;
	c->cli_StandardInput = std;
	c->cli_StandardOutput = stdout_;
	c->cli_CurrentOutput = curout;
	c->cli_CommandFile = file;
	c->cli_FailLevel = fail;
	me->pr_CIS = oin;
	me->pr_COS = oout;
	lock_free(me->pr_CurrentDir);
	me->pr_CurrentDir = odir;
	path_set_cwd(lock_path(odir));
	amem_free(nf);
	sh = saved;
	return rc;
}

/* amigafmt -c: the lines as a script, then (NewShell) the console */
int32_t shell_main(const char *text)
{
	struct CommandLineInterface *c = cli();
	char *tmpl;
	int fd = shell_mkstemp(&tmpl);
	size_t n = strlen(text);

	if (fd < 0 || write(fd, text, n) != (ssize_t)n ||
	    (n && text[n - 1] != '\n' && write(fd, "\n", 1) != 1)) {
		perror("amigafmt: shell");
		if (fd >= 0) {
			unlink(tmpl);
			free(tmpl);
			close(fd);
		}
		return 20;
	}
	unlink(tmpl);
	free(tmpl);
	lseek(fd, 0, SEEK_SET);
	dos_preload_args("", 0);        /* no argument line for anyone */
	c->cli_CurrentInput = fh_new(fd, -1, 0, NULL);
	c->cli_Interactive = 0;
	return run_script_input();
}

/* ---- the shell's own commands ---- */

static int32_t c_echo(const char *args)
{
	ARGS("/M,NOLINE/S,FIRST/K/N,LEN/K/N,TO/K", 5);
	struct sbuf b = { 0 };
	uint32_t *m = PTR(arg[0]);
	BPTR to = 0, oout = me->pr_COS;
	int32_t first = arg[2] ? *(int32_t *)PTR(arg[2]) : 0;
	int32_t len = arg[3] ? *(int32_t *)PTR(arg[3]) : -1;
	size_t start;

	sb_add(&b, "", 0);
	for (; m && *m; m++) {
		if (b.n)
			sb_addc(&b, ' ');
		sb_add(&b, PTR(*m), strlen(PTR(*m)));
	}
	start = first > 0 ? (size_t)first - 1 : 0;
	if (len >= 0 && !arg[2] && (size_t)len < b.n)
		start = b.n - len;      /* LEN alone: the last characters */
	if (start > b.n)
		start = b.n;
	if (len < 0 || start + len > b.n)
		len = b.n - start;
	if (arg[4]) {
		to = dos_open(PTR(arg[4]), MODE_NEWFILE);
		if (!to) {
			fault(me->pr_Result2, PTR(arg[4]));
			free(b.s);
			rdfree(rd);
			return 20;
		}
		me->pr_COS = to;
	}
	out("%.*s%s", (int)len, b.s + start, arg[1] ? "" : "\n");
	if (to) {
		me->pr_COS = oout;
		dos_close(to);
	}
	free(b.s);
	rdfree(rd);
	return 0;
}

static int32_t c_ask(const char *args)
{
	ARGS("PROMPT/A", 1);
	struct afh *in = fh_priv(me->pr_CIS);
	int32_t rc = 0;

	for (;;) {
		char ans[64];
		int c = 0, n = 0;
		out("%s ", (char *)PTR(arg[0]));
		dos_flush_all();
		while (in && (c = fh_getc(in)) >= 0 && c != '\n')
			if (n < 63)
				ans[n++] = c;
		ans[n] = 0;
		if (!strcasecmp(ans, "y") || !strcasecmp(ans, "yes")) {
			rc = 5;
			break;
		}
		if (!*ans || !strcasecmp(ans, "n") || !strcasecmp(ans, "no") ||
		    !in || c < 0)
			break;
	}
	rdfree(rd);
	return rc;
}

static int32_t c_cd(const char *args)
{
	ARGS("DIR", 1);
	char *lp;
	struct stat st;

	if (!arg[0]) {
		char *a = path_to_amiga(path_cwd());
		out("%s\n", a);
		free(a);
		rdfree(rd);
		return 0;
	}
	lp = path_to_linux(PTR(arg[0]), NULL);
	if (!lp || stat(lp, &st)) {
		out("Can't find %s\n", (char *)PTR(arg[0]));
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
		free(lp);
		rdfree(rd);
		return 20;
	}
	if (!S_ISDIR(st.st_mode)) {
		out("%s is not a directory\n", (char *)PTR(arg[0]));
		dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE);
		free(lp);
		rdfree(rd);
		return 20;
	}
	{
		BPTR old = me->pr_CurrentDir;
		char real[4096];
		if (!realpath(lp, real))
			snprintf(real, sizeof(real), "%s", lp);
		me->pr_CurrentDir = lock_new(real, SHARED_LOCK);
		path_set_cwd(real);
		if (old)
			lock_free(old);
		{
			char *a = path_to_amiga(real);
			bset(cli()->cli_SetName, a);
			free(a);
		}
	}
	free(lp);
	rdfree(rd);
	return 0;
}

static int32_t c_path(const char *args)
{
	ARGS("PATH/M,ADD/S,SHOW/S,RESET/S,REMOVE/S,QUIET/S", 6);
	struct CommandLineInterface *c = cli();
	uint32_t *m = PTR(arg[0]);
	BPTR *pp;
	int32_t rc = 0;

	if (arg[3]) {                                   /* RESET */
		while (c->cli_CommandDir) {
			BPTR *e = BADDR(c->cli_CommandDir);
			c->cli_CommandDir = e[0];
			lock_free(e[1]);
			amem_free(e);
		}
	}
	for (; m && *m; m++) {
		char *lp = path_to_linux(PTR(*m), NULL);
		struct stat st;
		if (!lp || stat(lp, &st) || !S_ISDIR(st.st_mode)) {
			if (!arg[5])
				out("Can't find %s\n", (char *)PTR(*m));
			free(lp);
			rc = 10;
			continue;
		}
		if (arg[4]) {                           /* REMOVE */
			for (pp = &c->cli_CommandDir; *pp;
			     pp = &((BPTR *)BADDR(*pp))[0]) {
				BPTR *e = BADDR(*pp);
				if (!strcmp(lock_path(e[1]), lp)) {
					*pp = e[0];
					lock_free(e[1]);
					amem_free(e);
					break;
				}
			}
		} else {
			BPTR *e = amem_alloc(8);
			for (pp = &c->cli_CommandDir; *pp;
			     pp = &((BPTR *)BADDR(*pp))[0])
				;
			e[1] = lock_new(lp, SHARED_LOCK);
			*pp = MKBADDR(e);
		}
		free(lp);
	}
	if (arg[2] || (!arg[0] && !arg[3])) {           /* SHOW */
		out("Current directory\n");
		for (BPTR p = c->cli_CommandDir; p; p = ((BPTR *)BADDR(p))[0]) {
			char *a = path_to_amiga(lock_path(((BPTR *)BADDR(p))[1]));
			out("%s\n", a);
			free(a);
		}
		out("C:\n");
	}
	rdfree(rd);
	return rc;
}

static int32_t c_prompt(const char *args)
{
	ARGS("PROMPT", 1);
	bset(cli()->cli_Prompt, arg[0] ? PTR(arg[0]) : "%N> ");
	rdfree(rd);
	return 0;
}

static int32_t c_stack(const char *args)
{
	ARGS("SIZE/N", 1);
	if (arg[0]) {
		int32_t sz = *(int32_t *)PTR(arg[0]);
		if (sz < 1600) {
			out("Stack size too small\n");
			rdfree(rd);
			return 10;
		}
		cli()->cli_DefaultStack = sz / 4;
	} else
		out("Current stack size is %d bytes\n",
		    (int)cli()->cli_DefaultStack * 4);
	rdfree(rd);
	return 0;
}

/* Set/Alias and Unset/Unalias: local variables of a type */
static int32_t set_local(const char *args, int type)
{
	ARGS("NAME,STRING/F", 2);
	if (!arg[0]) {
		struct LocalVar *v;
		for (v = PTR(((struct List *)&me->pr_LocalVars)->lh_Head);
		     v->lv_Node.ln_Succ; v = PTR(v->lv_Node.ln_Succ))
			if ((v->lv_Node.ln_Type & 0x7f) == type)
				out("%-16s %.*s\n", (char *)PTR(v->lv_Node.ln_Name),
				    (int)v->lv_Len, (char *)PTR(v->lv_Value));
	} else
		var_set_local(PTR(arg[0]), arg[1] ? PTR(arg[1]) : "", -1, type);
	rdfree(rd);
	return 0;
}

static int32_t unset_local(const char *args, int type)
{
	ARGS("NAME", 1);
	if (arg[0])
		var_delete_local(PTR(arg[0]), type);
	rdfree(rd);
	return 0;
}

static int32_t c_set(const char *args) { return set_local(args, LV_VAR); }
static int32_t c_alias(const char *args) { return set_local(args, LV_ALIAS); }
static int32_t c_unset(const char *args) { return unset_local(args, LV_VAR); }
static int32_t c_unalias(const char *args) { return unset_local(args, LV_ALIAS); }

static int32_t c_get(const char *args)
{
	ARGS("NAME/A", 1);
	struct LocalVar *v = var_find(PTR(arg[0]), LV_VAR);
	int32_t rc = 0;

	if (v)
		out("%.*s\n", (int)v->lv_Len, (char *)PTR(v->lv_Value));
	else {
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
		rc = 5;
	}
	rdfree(rd);
	return rc;
}

static int32_t c_setenv(const char *args)
{
	ARGS("NAME,STRING/F", 2);
	int32_t rc = 0;

	if (!arg[0]) {                  /* the global variables */
		char *lp = path_to_linux("ENV:", NULL);
		DIR *d = lp ? opendir(lp) : NULL;
		struct dirent *de;
		while (d && (de = readdir(d))) {
			int32_t len;
			char *v, *nl;
			if (de->d_name[0] == '.' || de->d_type == DT_DIR)
				continue;
			if (!(v = var_get_global(de->d_name, &len)))
				continue;
			nl = memchr(v, '\n', len);
			out("%-16s %.*s\n", de->d_name, (int)(nl ? nl - v : len), v);
			free(v);
		}
		if (d)
			closedir(d);
		free(lp);
	} else if (!var_set_global(PTR(arg[0]), arg[1] ? PTR(arg[1]) : "",
				   -1, 0)) {
		fault(me->pr_Result2, PTR(arg[0]));
		rc = 10;
	}
	rdfree(rd);
	return rc;
}

static int32_t c_unsetenv(const char *args)
{
	ARGS("NAME", 1);
	if (arg[0])
		var_delete_global(PTR(arg[0]));
	rdfree(rd);
	return 0;
}

static int32_t c_getenv(const char *args)
{
	ARGS("NAME/A", 1);
	int32_t len, rc = 0;
	char *v = var_get_global(PTR(arg[0]), &len);

	if (v) {
		char *nl = memchr(v, '\n', len);
		out("%.*s\n", (int)(nl ? nl - v : len), v);
		free(v);
	} else {
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
		rc = 5;
	}
	rdfree(rd);
	return rc;
}

static int32_t c_failat(const char *args)
{
	ARGS("RCLIM/N", 1);
	if (arg[0])
		cli()->cli_FailLevel = *(int32_t *)PTR(arg[0]);
	else
		out("Fail limit: %d\n", (int)cli()->cli_FailLevel);
	rdfree(rd);
	return 0;
}

static int32_t c_fault(const char *args)
{
	ARGS("/N/M", 1);
	int32_t **m = PTR(arg[0]);
	char buf[300];

	for (; m && *m; m++) {
		char hdr[32];
		snprintf(hdr, sizeof(hdr), "Fault %d", (int)**m);
		dos_fault_text(**m, hdr, buf, sizeof(buf));
		out("%s\n", buf);
	}
	rdfree(rd);
	return 0;
}

static int32_t c_why(const char *args)
{
	struct CommandLineInterface *c = cli();
	char buf[300];

	(void)args;
	if (!c->cli_ReturnCode || !c->cli_Result2)
		out("The last command did not set a return code\n");
	else {
		dos_fault_text(c->cli_Result2, "Last command failed because",
			       buf, sizeof(buf));
		out("%s\n", buf);
	}
	return 0;
}

/* IF: NOT, WARN/ERROR/FAIL on the last return code, a EQ/GT/GE b (VAL:
   as numbers), EXISTS file */
static int32_t c_if(const char *args)
{
	ARGS("NOT/S,WARN/S,ERROR/S,FAIL/S,,EQ/K,GT/K,GE/K,VAL/S,EXISTS/K", 10);
	int32_t rc = cli()->cli_ReturnCode;
	int cond = 1;

	if (arg[1])
		cond = rc >= 5;
	else if (arg[2])
		cond = rc >= 10;
	else if (arg[3])
		cond = rc >= 20;
	else if (arg[5] || arg[6] || arg[7]) {
		const char *a = arg[4] ? PTR(arg[4]) : "";
		const char *b = PTR(arg[5] ? arg[5] : arg[6] ? arg[6] : arg[7]);
		long d;
		if (arg[8])
			d = strtol(a, NULL, 0) - strtol(b, NULL, 0);
		else
			d = strcasecmp(a, b);
		cond = arg[5] ? d == 0 : arg[6] ? d > 0 : d >= 0;
	} else if (arg[9]) {
		char *lp = path_to_linux(PTR(arg[9]), NULL);
		struct stat st;
		cond = lp && !stat(lp, &st);
		free(lp);
	} else if (arg[4]) {
		cond = 0;       /* a bare word: nothing to compare it with */
	}
	if (arg[0])
		cond = !cond;
	if (!cond) {
		sh.skip_if = 1;
		sh.skip_depth = 0;
	}
	rdfree(rd);
	return rc;              /* IF leaves the return code alone */
}

static int32_t c_else(const char *args)
{
	(void)args;
	sh.skip_if = 2;         /* the true branch has run: skip to ENDIF */
	sh.skip_depth = 0;
	return cli()->cli_ReturnCode;
}

static int32_t c_endif(const char *args)
{
	(void)args;
	return cli()->cli_ReturnCode;
}

static int32_t c_skip(const char *args)
{
	ARGS("LABEL,BACK/S", 2);
	struct CommandLineInterface *c = cli();

	if (c->cli_CurrentInput == c->cli_StandardInput) {
		out("Skip must be in a command file\n");
		rdfree(rd);
		return 10;
	}
	snprintf(sh.label, sizeof(sh.label), "%s", arg[0] ? (char *)PTR(arg[0]) : "");
	sh.skip_label = 1;
	if (arg[1]) {                                   /* BACK: from the start */
		struct afh *a = fh_priv(c->cli_CurrentInput);
		if (a && a->seekable) {
			lseek(a->rfd, 0, SEEK_SET);
			a->rpos = a->rlen = 0;
			a->unget = -1;
		}
	}
	rdfree(rd);
	return 0;
}

static int32_t c_lab(const char *args) { (void)args; return 0; }

static int32_t c_quit(const char *args)
{
	ARGS("RC/N", 1);
	sh.quit = 1;
	sh.quit_rc = arg[0] ? *(int32_t *)PTR(arg[0]) : 0;
	rdfree(rd);
	return sh.quit_rc;
}

static int32_t c_endshell(const char *args)
{
	(void)args;
	sh.endshell = 1;
	return 0;
}

static int32_t c_newshell(const char *args)
{
	ARGS("WINDOW,FROM", 2);
	struct CommandLineInterface *c = cli();
	struct shstate saved = sh;
	BPTR cur = c->cli_CurrentInput, std = c->cli_StandardInput;
	BPTR from;
	int32_t rc;

	/* the console is the window; FROM (S:Shell-Startup) runs first.
	   An argument line left on Input() is not for the shell to read */
	{
		const char *a;
		int32_t l;
		dos_take_args(&a, &l);
	}
	memset(&sh, 0, sizeof(sh));
	c->cli_StandardInput = c->cli_CurrentInput = me->pr_CIS;
	from = dos_open(arg[1] ? PTR(arg[1]) : "S:Shell-Startup", MODE_OLDFILE);
	if (from)
		c->cli_CurrentInput = from;
	bset(c->cli_Prompt, "%N.%S> ");
	rdfree(rd);
	rc = shell_loop(1);
	c->cli_CurrentInput = cur;
	c->cli_StandardInput = std;
	sh = saved;
	return rc;
}

static int32_t c_run(const char *args)
{
	pid_t pid;

	dos_flush_all();
	pid = fork();
	if (pid < 0) {
		fault(dos_errno(errno), "Run");
		return 20;
	}
	if (pid == 0) {
		/* the child is the new CLI */
		me->pr_TaskNum = 2;
		shell_line(args);
		dos_flush_all();
		_exit(0);
	}
	out("[CLI 2]\n");
	return 0;
}

static int32_t c_resident(const char *args)
{
	ARGS("NAME,FILE,REMOVE/S,ADD/S,REPLACE/S,PURE=FORCE/S,SYSTEM/S", 7);
	int32_t rc = 0;

	if (!arg[0] && !arg[1]) {
		out("NAME                           USECOUNT\n\n");
		for (struct Segment *s = BADDR(seg_head); s; s = BADDR(s->seg_Next)) {
			char n[256];
			memcpy(n, s->seg_Name + 1, s->seg_Name[0]);
			n[s->seg_Name[0]] = 0;
			if (s->seg_UC == CMD_INTERNAL)
				out("%-30s INTERNAL\n", n);
			else if (s->seg_UC == CMD_SYSTEM)
				out("%-30s SYSTEM\n", n);
			else if (s->seg_UC >= 0)
				out("%-30s %d\n", n, (int)s->seg_UC);
		}
	} else if (arg[2]) {                            /* REMOVE */
		struct Segment *s = shell_find_segment(PTR(arg[0]), NULL, 0);
		if (!s || !seg_remove(s)) {
			out("Can't remove %s\n", (char *)PTR(arg[0]));
			rc = 10;
		}
	} else {
		const char *file = arg[1] ? PTR(arg[1]) : PTR(arg[0]);
		const char *name = arg[0] ? PTR(arg[0]) : PTR(arg[1]);
		char *lp = find_command(file);
		const char *base = strrchr(name, '/');
		int err;
		BPTR seg;

		if (!base)
			base = strrchr(name, ':');
		base = base ? base + 1 : name;
		seg = lp ? hunk_load(lp, &err) : 0;
		free(lp);
		if (!seg) {
			out("Can't load %s\n", file);
			rc = 10;
		} else {
			struct Segment *old = shell_find_segment(base, NULL, 0);
			struct Segment *s;
			if (old && !arg[4]) {
				out("%s is already resident\n", base);
				hunk_unload(seg);
				rc = 10;
			} else {
				if (old)
					seg_remove(old);
				s = seg_new(base, seg, arg[6] ? CMD_SYSTEM : 0);
				seg_push(s);
			}
		}
	}
	rdfree(rd);
	return rc;
}

/* ---- the internal commands' resident list entries ---- */

static const struct {
	const char *name;
	int32_t (*fn)(const char *);
} internal[] = {
	{ "Alias", c_alias }, { "Ask", c_ask }, { "CD", c_cd },
	{ "Echo", c_echo }, { "Else", c_else }, { "EndCLI", c_endshell },
	{ "EndIf", c_endif }, { "EndShell", c_endshell }, { "EndSkip", c_lab },
	{ "FailAt", c_failat }, { "Fault", c_fault }, { "Get", c_get },
	{ "Getenv", c_getenv }, { "If", c_if }, { "Lab", c_lab },
	{ "NewCLI", c_newshell }, { "NewShell", c_newshell },
	{ "Path", c_path }, { "Prompt", c_prompt }, { "Quit", c_quit },
	{ "Resident", c_resident }, { "Run", c_run }, { "Set", c_set },
	{ "Setenv", c_setenv }, { "Skip", c_skip }, { "Stack", c_stack },
	{ "Unalias", c_unalias }, { "Unset", c_unset },
	{ "Unsetenv", c_unsetenv }, { "Why", c_why },
};
#define NINTERNAL (int)(sizeof(internal) / sizeof(internal[0]))

static struct Segment *internal_seg[NINTERNAL];

static int32_t (*internal_fn(struct Segment *s))(const char *)
{
	for (int i = 0; i < NINTERNAL; i++)
		if (internal_seg[i] == s)
			return internal[i].fn;
	return NULL;
}

/* entered through the hidden library's vector: a0 = arguments */
static void internal_entry(int i, struct frame *f)
{
	const char *a = PTR(A(0));
	char *args = strndup(a ? a : "", D(0));

	RET(internal[i].fn(args));
	free(args);
}

#define ENTRY(n) static void e##n(struct frame *f) { internal_entry(n, f); }
ENTRY(0) ENTRY(1) ENTRY(2) ENTRY(3) ENTRY(4) ENTRY(5) ENTRY(6) ENTRY(7)
ENTRY(8) ENTRY(9) ENTRY(10) ENTRY(11) ENTRY(12) ENTRY(13) ENTRY(14)
ENTRY(15) ENTRY(16) ENTRY(17) ENTRY(18) ENTRY(19) ENTRY(20) ENTRY(21)
ENTRY(22) ENTRY(23) ENTRY(24) ENTRY(25) ENTRY(26) ENTRY(27) ENTRY(28)
ENTRY(29)
_Static_assert(NINTERNAL == 30, "one entry per internal command");

static const struct fdent fd_shellcmd[] = {
	{ 30, "Cmd0", "a0/d0" }, { 36, "Cmd1", "a0/d0" }, { 42, "Cmd2", "a0/d0" },
	{ 48, "Cmd3", "a0/d0" }, { 54, "Cmd4", "a0/d0" }, { 60, "Cmd5", "a0/d0" },
	{ 66, "Cmd6", "a0/d0" }, { 72, "Cmd7", "a0/d0" }, { 78, "Cmd8", "a0/d0" },
	{ 84, "Cmd9", "a0/d0" }, { 90, "Cmd10", "a0/d0" }, { 96, "Cmd11", "a0/d0" },
	{ 102, "Cmd12", "a0/d0" }, { 108, "Cmd13", "a0/d0" },
	{ 114, "Cmd14", "a0/d0" }, { 120, "Cmd15", "a0/d0" },
	{ 126, "Cmd16", "a0/d0" }, { 132, "Cmd17", "a0/d0" },
	{ 138, "Cmd18", "a0/d0" }, { 144, "Cmd19", "a0/d0" },
	{ 150, "Cmd20", "a0/d0" }, { 156, "Cmd21", "a0/d0" },
	{ 162, "Cmd22", "a0/d0" }, { 168, "Cmd23", "a0/d0" },
	{ 174, "Cmd24", "a0/d0" }, { 180, "Cmd25", "a0/d0" },
	{ 186, "Cmd26", "a0/d0" }, { 192, "Cmd27", "a0/d0" },
	{ 198, "Cmd28", "a0/d0" }, { 204, "Cmd29", "a0/d0" },
	{ 0, 0, 0 }
};

static const struct libimpl shellcmd_impl[] = {
	{ "Cmd0", e0 }, { "Cmd1", e1 }, { "Cmd2", e2 }, { "Cmd3", e3 },
	{ "Cmd4", e4 }, { "Cmd5", e5 }, { "Cmd6", e6 }, { "Cmd7", e7 },
	{ "Cmd8", e8 }, { "Cmd9", e9 }, { "Cmd10", e10 }, { "Cmd11", e11 },
	{ "Cmd12", e12 }, { "Cmd13", e13 }, { "Cmd14", e14 }, { "Cmd15", e15 },
	{ "Cmd16", e16 }, { "Cmd17", e17 }, { "Cmd18", e18 }, { "Cmd19", e19 },
	{ "Cmd20", e20 }, { "Cmd21", e21 }, { "Cmd22", e22 }, { "Cmd23", e23 },
	{ "Cmd24", e24 }, { "Cmd25", e25 }, { "Cmd26", e26 }, { "Cmd27", e27 },
	{ "Cmd28", e28 }, { "Cmd29", e29 },
	{ NULL, NULL }
};

struct amilib lib_shellcmd = {
	.name = "shell commands", .version = 40, .revision = 1,
	.fd = fd_shellcmd, .impl = shellcmd_impl, .hidden = 1,
};

/* the resident list starts with the internal commands, each a seglist
   whose code jumps to its vector */
void shell_init(void)
{
	struct Library *b = lib_build(&lib_shellcmd);

	for (int i = NINTERNAL - 1; i >= 0; i--) {
		uint32_t *sl = amem_alloc(16);
		uint16_t *code = (uint16_t *)(sl + 1);
		uint32_t vec = APTR_OF(b) - (5 + i) * 6;

		code[0] = 0x4ef9;               /* jmp vector */
		code[1] = vec >> 16;
		code[2] = vec;
		cache_flush(sl, 16);
		internal_seg[i] = seg_new(internal[i].name, MKBADDR(sl),
					  CMD_INTERNAL);
		seg_push(internal_seg[i]);
	}
}

const struct libimpl dos_impl_shell[] = {
	{ "FindSegment", d_FindSegment }, { "AddSegment", d_AddSegment },
	{ "RemSegment", d_RemSegment },
	{ NULL, NULL }
};
