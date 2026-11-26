/*
 * dos.library variables (dos/var.h).
 *
 * Local variables and aliases are LocalVar nodes on the process's
 * pr_LocalVars list, kept in alphabetical order as the autodoc asks.
 * Global variables are files in ENV: (and ENVARC: with GVF_SAVE_VAR);
 * GetVar falls back to the Linux environment for a global it does not
 * find there, so $HOME and friends are visible.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dos_int.h"

#define GVF_GLOBAL_ONLY   0x100
#define GVF_LOCAL_ONLY    0x200
#define GVF_BINARY_VAR    0x400
#define GVF_DONT_NULL_TERM 0x800
#define GVF_SAVE_VAR      0x1000
#define LVF_IGNORE        0x80

struct LocalVar {
	struct Node lv_Node;
	uint16_t lv_Flags;
	APTR lv_Value;
	uint32_t lv_Len;
};

static struct List *locals(void)
{
	return (struct List *)&me->pr_LocalVars;
}

struct LocalVar *var_find(const char *name, int type)
{
	struct LocalVar *v;

	for (v = PTR(locals()->lh_Head); v->lv_Node.ln_Succ;
	     v = PTR(v->lv_Node.ln_Succ))
		if ((v->lv_Node.ln_Type & ~LVF_IGNORE) == type &&
		    !(v->lv_Node.ln_Type & LVF_IGNORE) &&
		    !strcasecmp(PTR(v->lv_Node.ln_Name), name))
			return v;
	return NULL;
}

static void var_free(struct LocalVar *v)
{
	exec_remove(v);
	amem_free(PTR(v->lv_Value));
	amem_free(PTR(v->lv_Node.ln_Name));
	amem_free(v);
}

int var_set_local(const char *name, const char *val, int32_t len, int type)
{
	struct LocalVar *v = var_find(name, type), *p;
	char *nm, *value;

	if (len < 0)
		len = strlen(val);
	value = amem_alloc(len + 1);
	if (!value) {
		dos_set_ioerr(ERROR_NO_FREE_STORE);
		return 0;
	}
	memcpy(value, val, len);
	if (v) {
		amem_free(PTR(v->lv_Value));
		v->lv_Value = APTR_OF(value);
		v->lv_Len = len;
		return 1;
	}
	v = amem_alloc(sizeof(*v));
	nm = amem_alloc(strlen(name) + 1);
	if (!v || !nm) {
		amem_free(v);
		amem_free(nm);
		amem_free(value);
		dos_set_ioerr(ERROR_NO_FREE_STORE);
		return 0;
	}
	strcpy(nm, name);
	v->lv_Node.ln_Type = type;
	v->lv_Node.ln_Name = APTR_OF(nm);
	v->lv_Value = APTR_OF(value);
	v->lv_Len = len;
	/* alphabetical: before the first entry whose name sorts after */
	for (p = PTR(locals()->lh_Head); p->lv_Node.ln_Succ;
	     p = PTR(p->lv_Node.ln_Succ))
		if (strcasecmp(PTR(p->lv_Node.ln_Name), name) > 0)
			break;
	v->lv_Node.ln_Succ = APTR_OF(p);
	v->lv_Node.ln_Pred = p->lv_Node.ln_Pred;
	((struct Node *)PTR(p->lv_Node.ln_Pred))->ln_Succ = APTR_OF(v);
	p->lv_Node.ln_Pred = APTR_OF(v);
	return 1;
}

int var_delete_local(const char *name, int type)
{
	struct LocalVar *v = var_find(name, type);

	if (!v)
		return 0;
	var_free(v);
	return 1;
}

/* the Linux path of ENV:name (or ENVARC:name) */
static char *env_path(const char *dir, const char *name)
{
	char a[512];

	snprintf(a, sizeof(a), "%s:%s", dir, name);
	return path_to_linux(a, NULL);
}

/* a global's value, malloc()ed, or NULL */
char *var_get_global(const char *name, int32_t *len)
{
	char *lp = env_path("ENV", name), *buf = NULL;
	const char *e;
	FILE *fp;

	if (lp && (fp = fopen(lp, "rb"))) {
		size_t cap = 256, n = 0, r;
		buf = malloc(cap);
		while ((r = fread(buf + n, 1, cap - n, fp)) > 0) {
			n += r;
			if (n == cap)
				buf = realloc(buf, cap *= 2);
		}
		fclose(fp);
		*len = n;
	} else if ((e = getenv(name))) {
		buf = strdup(e);
		*len = strlen(e);
	}
	free(lp);
	return buf;
}

static int write_file(const char *lp, const char *val, int32_t len)
{
	FILE *fp;
	char *dir = strdup(lp), *s = strrchr(dir, '/');

	if (s) {                        /* ENV: itself may not exist yet */
		*s = 0;
		mkdir(dir, 0777);
	}
	free(dir);
	fp = fopen(lp, "wb");
	if (!fp)
		return 0;
	if (len && fwrite(val, 1, len, fp) != (size_t)len) {
		fclose(fp);
		return 0;
	}
	return !fclose(fp);
}

int var_set_global(const char *name, const char *val, int32_t len, int save)
{
	char *lp = env_path("ENV", name);
	int ok;

	if (len < 0)
		len = strlen(val);
	ok = lp && write_file(lp, val, len);
	if (!ok)
		dos_set_ioerr(lp ? dos_errno(errno) : ERROR_DEVICE_NOT_MOUNTED);
	free(lp);
	if (ok && save) {
		lp = env_path("ENVARC", name);
		ok = lp && write_file(lp, val, len);
		free(lp);
	}
	return ok;
}

int var_delete_global(const char *name)
{
	char *lp = env_path("ENV", name);
	int ok = lp && !unlink(lp);

	free(lp);
	if (getenv(name)) {
		unsetenv(name);
		ok = 1;
	}
	return ok;
}

static void d_GetVar(struct frame *f)
{
	const char *name = PTR(D(1));
	char *buf = PTR(D(2)), *g = NULL;
	int32_t size = D(3), len = 0, n;
	uint32_t flags = D(4);
	const char *val = NULL;
	struct LocalVar *v = NULL;

	if (size <= 0) {
		dos_set_ioerr(ERROR_BAD_NUMBER);
		RET(-1);
		return;
	}
	if (!(flags & GVF_GLOBAL_ONLY) && (v = var_find(name, flags & 0xff))) {
		val = PTR(v->lv_Value);
		len = v->lv_Len;
	} else if (!(flags & GVF_LOCAL_ONLY) && (flags & 0xff) == 0 &&
		   (g = var_get_global(name, &len)))
		val = g;
	if (!val) {
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
		RET(-1);
		return;
	}
	n = len;
	if (!(flags & GVF_BINARY_VAR)) {
		const char *nl = memchr(val, '\n', len);
		if (nl)
			n = nl - val;
	}
	if (flags & GVF_DONT_NULL_TERM && flags & GVF_BINARY_VAR) {
		if (n > size)
			n = size;
	} else {
		if (n > size - 1)
			n = size - 1;
		buf[n] = 0;
	}
	memcpy(buf, val, n);
	free(g);
	dos_set_ioerr(len);
	RET(n);
}

static void d_SetVar(struct frame *f)
{
	const char *name = PTR(D(1)), *val = PTR(D(2));
	int32_t size = D(3);
	uint32_t flags = D(4);
	int ok;

	if (!val) {                     /* a NULL buffer deletes it */
		ok = flags & GVF_GLOBAL_ONLY ? var_delete_global(name) :
		     var_delete_local(name, flags & 0xff);
		RET(ok ? DOSTRUE : DOSFALSE);
		return;
	}
	if (flags & GVF_GLOBAL_ONLY) {
		if ((flags & 0xff) != 0) {      /* only LV_VAR can be global */
			dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE);
			RET(DOSFALSE);
			return;
		}
		ok = var_set_global(name, val, size, flags & GVF_SAVE_VAR);
	} else
		ok = var_set_local(name, val, size, flags & 0xff);
	RET(ok ? DOSTRUE : DOSFALSE);
}

static void d_DeleteVar(struct frame *f)
{
	const char *name = PTR(D(1));
	uint32_t flags = D(2);
	int ok = 0;

	if (!(flags & GVF_GLOBAL_ONLY))
		ok = var_delete_local(name, flags & 0xff);
	if (!ok && !(flags & GVF_LOCAL_ONLY) && (flags & 0xff) == 0)
		ok = var_delete_global(name);
	if (!ok)
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
	RET(ok ? DOSTRUE : DOSFALSE);
}

static void d_FindVar(struct frame *f)
{
	RET(var_find(PTR(D(1)), D(2) & 0xff));
}

const struct libimpl dos_impl_var[] = {
	{ "GetVar", d_GetVar }, { "SetVar", d_SetVar },
	{ "DeleteVar", d_DeleteVar }, { "FindVar", d_FindVar },
	{ NULL, NULL }
};
