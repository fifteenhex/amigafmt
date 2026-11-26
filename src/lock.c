/*
 * dos.library: locks, directories, the DOS list and MatchFirst/MatchNext.
 *
 * A lock is a real FileLock (first member of struct alock, so the BPTR
 * points where programs expect) followed by our Linux path and directory
 * stream.  A zero lock means the root of SYS:, as on AmigaOS.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#include "af.h"
#include "dos_int.h"

struct alock {
	struct FileLock fl;
	char *path;
	DIR *dir;               /* Examine/ExNext and ExAll state */
	char *pending;          /* an ExAll entry that did not fit */
	uint32_t magic;
};

#define LOCK_MAGIC 0x41464c4b

static char sysroot[PATH_MAX];

static struct alock *AL(BPTR b)
{
	struct alock *l = BADDR(b);

	if (!b || l->magic != LOCK_MAGIC)
		return NULL;
	return l;
}

const char *lock_path(BPTR b)
{
	struct alock *l = AL(b);
	char *p;

	if (l)
		return l->path;
	if (!sysroot[0] && (p = path_to_linux("SYS:", "/"))) {
		snprintf(sysroot, sizeof(sysroot), "%s", p);
		free(p);
	}
	return sysroot;
}

/* the directory relative names start from */
static const char *base(void)
{
	return me ? lock_path(me->pr_CurrentDir) : path_cwd();
}

static char *resolve(const char *name)
{
	char *p = path_to_linux(name ? name : "", base());

	if (!p)
		dos_set_ioerr(ERROR_DEVICE_NOT_MOUNTED);
	return p;
}

struct dlent {
	uint8_t node[44];
	uint8_t name[36];
};

static void dl_build(void);
static struct dlent *dl_head;
static int dl_count;

/* the DosList volume a Linux path is on (SYS, RAM, ROOT) */
static BPTR volume_node(const char *lpath)
{
	char *a = path_to_amiga(lpath), *c = strchr(a, ':');
	BPTR r = 0;
	int i;

	if (c)
		*c = 0;
	dl_build();
	for (i = 0; i < dl_count; i++)
		if (*(int32_t *)(dl_head[i].node + 4) == 2 &&
		    dl_head[i].name[0] == strlen(a) &&
		    !strncasecmp((char *)dl_head[i].name + 1, a, strlen(a)))
			r = MKBADDR(dl_head[i].node);
	free(a);
	return r;
}

BPTR lock_new(const char *lpath, int32_t mode)
{
	struct alock *l;
	struct stat st;

	if (lstat(lpath, &st) && stat(lpath, &st)) {
		dos_set_ioerr(dos_errno(errno));
		return 0;
	}
	l = amem_alloc(sizeof(*l));
	l->magic = LOCK_MAGIC;
	l->path = strdup(lpath);
	l->fl.fl_Key = st.st_ino;
	l->fl.fl_Access = mode;
	l->fl.fl_Task = me ? APTR_OF(&me->pr_MsgPort) : 0;
	l->fl.fl_Volume = volume_node(lpath);
	return MKBADDR(l);
}

void lock_free(BPTR b)
{
	struct alock *l = AL(b);

	if (!l)
		return;
	if (l->dir)
		closedir(l->dir);
	free(l->pending);
	free(l->path);
	l->magic = 0;
	amem_free(l);
}

void lock_init_process(struct Process *p, const char *prog)
{
	char *pd = path_to_linux("PROGDIR:", "/");

	p->pr_CurrentDir = lock_new(path_cwd(), SHARED_LOCK);
	if (pd) {
		p->pr_HomeDir = lock_new(pd, SHARED_LOCK);
		free(pd);
	}
	(void)prog;
}

static void d_Lock(struct frame *f)
{
	char *p = resolve(PTR(D(1)));

	if (!p) {
		RET(0);
		return;
	}
	RET(lock_new(p, D(2)));
	if (f->d[0])
		dos_set_ioerr(0);
	free(p);
}

static void d_UnLock(struct frame *f)
{
	lock_free(D(1));
	RET(DOSTRUE);
}

static void d_DupLock(struct frame *f)
{
	if (!D(1)) {
		RET(0);
		return;
	}
	RET(lock_new(lock_path(D(1)), SHARED_LOCK));
}

static void d_CurrentDir(struct frame *f)
{
	BPTR old = me->pr_CurrentDir;

	me->pr_CurrentDir = D(1);
	path_set_cwd(lock_path(D(1)));
	RET(old);
}

static void d_ParentDir(struct frame *f)
{
	char *p = strdup(lock_path(D(1))), *s;
	char *a = path_to_amiga(p);

	/* the root of a volume has no parent */
	if (a[strlen(a) - 1] == ':' || !strcmp(p, "/")) {
		free(a);
		free(p);
		dos_set_ioerr(0);
		RET(0);
		return;
	}
	free(a);
	s = strrchr(p, '/');
	if (s == p)
		s[1] = 0;
	else if (s)
		*s = 0;
	RET(lock_new(p, SHARED_LOCK));
	free(p);
}

/* ---- FileInfoBlock ---- */

static int32_t prot_of(const struct stat *st)
{
	int32_t p = 0;

	if (!(st->st_mode & S_IRUSR))
		p |= FIBF_READ;
	if (!(st->st_mode & S_IWUSR))
		p |= FIBF_WRITE | FIBF_DELETE;
	if (!(st->st_mode & S_IXUSR))
		p |= FIBF_EXECUTE;
	return p;
}

static void fill_fib(struct FileInfoBlock *fib, const char *lpath,
		     const char *name, const struct stat *st, int root)
{
	memset(fib, 0, sizeof(*fib));
	fib->fib_DiskKey = st->st_ino;
	if (root)
		fib->fib_DirEntryType = ST_ROOT;
	else if (S_ISLNK(st->st_mode))
		fib->fib_DirEntryType = ST_SOFTLINK;
	else if (S_ISDIR(st->st_mode))
		fib->fib_DirEntryType = ST_USERDIR;
	else if (S_ISFIFO(st->st_mode))
		fib->fib_DirEntryType = ST_PIPEFILE;
	else
		fib->fib_DirEntryType = ST_FILE;
	fib->fib_EntryType = fib->fib_DirEntryType;
	snprintf(fib->fib_FileName, sizeof(fib->fib_FileName), "%s", name);
	fib->fib_Protection = prot_of(st);
	fib->fib_Size = S_ISDIR(st->st_mode) ? 0 : st->st_size;
	fib->fib_NumBlocks = (st->st_size + 511) / 512;
	date_from_unix(st->st_mtime, &fib->fib_Date);
	fib->fib_OwnerUID = st->st_uid;
	fib->fib_OwnerGID = st->st_gid;
	(void)lpath;
}

/* stat that follows links to directories, so they list as directories */
static int stat_entry(const char *path, struct stat *st)
{
	if (lstat(path, st))
		return -1;
	if (S_ISLNK(st->st_mode)) {
		struct stat t;
		if (!stat(path, &t))
			*st = t;
	}
	return 0;
}

static int examine_path(const char *lpath, struct FileInfoBlock *fib)
{
	struct stat st;
	char *a, *name;
	int root;

	if (stat(lpath, &st) && lstat(lpath, &st)) {
		dos_set_ioerr(dos_errno(errno));
		return 0;
	}
	a = path_to_amiga(lpath);
	root = a[strlen(a) - 1] == ':';
	if (root) {
		a[strlen(a) - 1] = 0;
		name = a;
	} else {
		name = strrchr(lpath, '/');
		name = name ? name + 1 : (char *)lpath;
	}
	fill_fib(fib, lpath, name, &st, root);
	free(a);
	return 1;
}

static void d_Examine(struct frame *f)
{
	struct alock *l = AL(D(1));
	const char *p = lock_path(D(1));

	if (l && l->dir) {              /* restart any ExNext scan */
		closedir(l->dir);
		l->dir = NULL;
	}
	RET(examine_path(p, PTR(D(2))) ? DOSTRUE : DOSFALSE);
}

static struct dirent *next_entry(DIR *d)
{
	struct dirent *e;

	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			return e;
	return NULL;
}

static void d_ExNext(struct frame *f)
{
	struct alock *l = AL(D(1));
	struct FileInfoBlock *fib = PTR(D(2));
	struct dirent *e;
	char full[PATH_MAX];
	struct stat st;

	if (!l) {
		/* a zero lock: give it a real one for the scan */
		dos_set_ioerr(ERROR_INVALID_LOCK);
		RET(DOSFALSE);
		return;
	}
	if (!l->dir && !(l->dir = opendir(l->path))) {
		dos_set_ioerr(errno == ENOTDIR ? ERROR_OBJECT_WRONG_TYPE :
			      dos_errno(errno));
		RET(DOSFALSE);
		return;
	}
	for (;;) {
		e = next_entry(l->dir);
		if (!e) {
			closedir(l->dir);
			l->dir = NULL;
			dos_set_ioerr(ERROR_NO_MORE_ENTRIES);
			RET(DOSFALSE);
			return;
		}
		snprintf(full, sizeof(full), "%s/%s", l->path, e->d_name);
		if (!stat_entry(full, &st))
			break;
	}
	fill_fib(fib, full, e->d_name, &st, 0);
	RET(DOSTRUE);
}

static void d_ExamineFH(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	struct FileInfoBlock *fib = PTR(D(2));
	struct stat st;
	const char *n;

	if (!a || fstat(a->rfd >= 0 ? a->rfd : a->wfd, &st)) {
		dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE);
		RET(DOSFALSE);
		return;
	}
	n = a->path ? strrchr(a->path, '/') : NULL;
	fill_fib(fib, a->path, n ? n + 1 : "", &st, 0);
	RET(DOSTRUE);
}

/* ---- ExAll ---- */

static const uint8_t ed_size[] = { 0, 8, 12, 16, 20, 32, 36, 40 };

struct eac_hook {
	struct frame *f;
	uint32_t hook;
};

static int exall_put(uint8_t **pos, uint8_t *end, uint8_t **prev,
		     const char *dir, const char *name, int32_t type,
		     struct ExAllControl *eac, struct frame *f)
{
	char full[PATH_MAX];
	struct stat st;
	size_t nl = strlen(name) + 1;
	uint32_t need = (ed_size[type] + nl + 1 + 3) & ~3u;
	uint8_t *ed = *pos;

	snprintf(full, sizeof(full), "%s/%s", dir, name);
	if (stat_entry(full, &st))
		return 1;               /* vanished: skip it */
	if (eac->eac_MatchString && !pat_match(PTR(eac->eac_MatchString), name, 1))
		return 1;
	if (ed + need > end)
		return 0;
	memset(ed, 0, need);
	*(uint32_t *)(ed + 4) = APTR_OF(ed + ed_size[type]);
	strcpy((char *)ed + ed_size[type], name);
	if (type >= 2)
		*(int32_t *)(ed + 8) = S_ISLNK(st.st_mode) ? ST_SOFTLINK :
				       S_ISDIR(st.st_mode) ? ST_USERDIR : ST_FILE;
	if (type >= 3)
		*(uint32_t *)(ed + 12) = S_ISDIR(st.st_mode) ? 0 : st.st_size;
	if (type >= 4)
		*(int32_t *)(ed + 16) = prot_of(&st);
	if (type >= 5)
		date_from_unix(st.st_mtime, (struct DateStamp *)(ed + 20));
	if (type >= 6)          /* empty comment: point at the name's NUL */
		*(uint32_t *)(ed + 32) = APTR_OF(ed + ed_size[type] + nl - 1);
	if (type >= 7) {
		*(uint16_t *)(ed + 36) = st.st_uid;
		*(uint16_t *)(ed + 38) = st.st_gid;
	}
	if (eac->eac_MatchFunc) {
		struct Hook *h = PTR(eac->eac_MatchFunc);
		struct regs r;
		int32_t t = type;
		regs_caller(&r, f);
		r.a[0] = APTR_OF(h);
		r.a[1] = APTR_OF(ed);
		r.a[2] = APTR_OF(&t);
		r.a[6] = APTR_OF(DOSBase);
		if (!amiga_call(f, h->h_Entry, &r))
			return 1;
	}
	if (*prev)
		*(uint32_t *)*prev = APTR_OF(ed);
	*prev = ed;
	*pos = ed + need;
	eac->eac_Entries++;
	return 1;
}

static void d_ExAll(struct frame *f)
{
	struct alock *l = AL(D(1));
	uint8_t *buf = PTR(D(2)), *pos = buf, *end = buf + D(3), *prev = NULL;
	int32_t type = D(4);
	struct ExAllControl *eac = PTR(D(5));
	struct dirent *e;

	eac->eac_Entries = 0;
	if (!l) {
		dos_set_ioerr(ERROR_INVALID_LOCK);
		RET(DOSFALSE);
		return;
	}
	if (type < 1 || type > 7) {
		dos_set_ioerr(ERROR_BAD_NUMBER);
		RET(DOSFALSE);
		return;
	}
	if (!eac->eac_LastKey || !l->dir) {
		if (l->dir)
			closedir(l->dir);
		free(l->pending);
		l->pending = NULL;
		l->dir = opendir(l->path);
		if (!l->dir) {
			dos_set_ioerr(errno == ENOTDIR ? ERROR_OBJECT_WRONG_TYPE :
				      dos_errno(errno));
			RET(DOSFALSE);
			return;
		}
	}
	eac->eac_LastKey++;
	if (l->pending) {
		if (!exall_put(&pos, end, &prev, l->path, l->pending, type, eac, f)) {
			dos_set_ioerr(ERROR_NO_FREE_STORE);
			RET(DOSFALSE);
			return;
		}
		free(l->pending);
		l->pending = NULL;
	}
	while ((e = next_entry(l->dir))) {
		if (!exall_put(&pos, end, &prev, l->path, e->d_name, type, eac, f)) {
			l->pending = strdup(e->d_name);
			RET(DOSTRUE);           /* call again for more */
			return;
		}
	}
	closedir(l->dir);
	l->dir = NULL;
	eac->eac_LastKey = 0;
	dos_set_ioerr(ERROR_NO_MORE_ENTRIES);
	RET(DOSFALSE);
}

static void d_ExAllEnd(struct frame *f)
{
	struct alock *l = AL(D(1));
	struct ExAllControl *eac = PTR(D(5));

	if (l && l->dir) {
		closedir(l->dir);
		l->dir = NULL;
	}
	if (l) {
		free(l->pending);
		l->pending = NULL;
	}
	if (eac)
		eac->eac_LastKey = 0;
}

/* ---- Info ---- */

int path_info(const char *p, struct InfoData *id)
{
	struct statvfs sv;

	memset(id, 0, sizeof(*id));
	if (statvfs(p, &sv)) {
		dos_set_ioerr(dos_errno(errno));
		return 0;
	}
	id->id_DiskState = (sv.f_flag & ST_RDONLY) ? 80 : 82;
	id->id_BytesPerBlock = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
	id->id_NumBlocks = sv.f_blocks;
	id->id_NumBlocksUsed = sv.f_blocks - sv.f_bfree;
	id->id_DiskType = 0x444f5300;
	id->id_VolumeNode = volume_node(p);
	id->id_InUse = DOSTRUE;
	return 1;
}

int lock_info(BPTR lock, struct InfoData *id)
{
	return path_info(lock_path(lock), id);
}

static void d_Info(struct frame *f)
{
	RET(lock_info(D(1), PTR(D(2))) ? DOSTRUE : DOSFALSE);
}

/* ---- names, creation, deletion ---- */

static void d_NameFromLock(struct frame *f)
{
	char *a = path_to_amiga(lock_path(D(1)));
	int32_t len = D(3);

	if ((int32_t)strlen(a) >= len) {
		if (len > 0)
			((char *)PTR(D(2)))[0] = 0;
		dos_set_ioerr(ERROR_LINE_TOO_LONG);
		RET(DOSFALSE);
	} else {
		strcpy(PTR(D(2)), a);
		RET(DOSTRUE);
	}
	free(a);
}

static void d_NameFromFH(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	char *n;

	if (!a || !a->path) {
		dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE);
		RET(DOSFALSE);
		return;
	}
	n = path_to_amiga(a->path);
	if ((int32_t)strlen(n) >= (int32_t)D(3)) {
		dos_set_ioerr(ERROR_LINE_TOO_LONG);
		RET(DOSFALSE);
	} else {
		strcpy(PTR(D(2)), n);
		RET(DOSTRUE);
	}
	free(n);
}

static void d_DupLockFromFH(struct frame *f)
{
	struct afh *a = fh_priv(D(1));

	RET(a && a->path ? lock_new(a->path, SHARED_LOCK) : 0);
}

static void d_ParentOfFH(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	char *p, *s;

	if (!a || !a->path) {
		RET(0);
		return;
	}
	p = strdup(a->path);
	s = strrchr(p, '/');
	if (s == p)
		s[1] = 0;
	else if (s)
		*s = 0;
	RET(lock_new(p, SHARED_LOCK));
	free(p);
}

static void d_OpenFromLock(struct frame *f)
{
	const char *p = lock_path(D(1));
	int fd = open(p, O_RDWR);
	BPTR b;

	if (fd < 0)
		fd = open(p, O_RDONLY);
	if (fd < 0) {
		dos_set_ioerr(dos_errno(errno));
		RET(0);
		return;
	}
	b = fh_new(fd, fd, 0, p);
	lock_free(D(1));
	RET(b);
}

static void d_CreateDir(struct frame *f)
{
	char *p = resolve(PTR(D(1)));

	if (!p) {
		RET(0);
		return;
	}
	if (mkdir(p, 0777)) {
		dos_set_ioerr(dos_errno(errno));
		RET(0);
	} else
		RET(lock_new(p, EXCLUSIVE_LOCK));
	free(p);
}

static void d_DeleteFile(struct frame *f)
{
	char *p = resolve(PTR(D(1)));
	struct stat st;

	if (!p) {
		RET(DOSFALSE);
		return;
	}
	if (!lstat(p, &st) && S_ISDIR(st.st_mode) ? rmdir(p) : unlink(p)) {
		dos_set_ioerr(errno == EEXIST ? ERROR_DIRECTORY_NOT_EMPTY :
			      errno == EACCES || errno == EPERM ?
			      ERROR_DELETE_PROTECTED : dos_errno(errno));
		RET(DOSFALSE);
	} else
		RET(DOSTRUE);
	free(p);
}

static void d_Rename(struct frame *f)
{
	char *a = resolve(PTR(D(1))), *b = resolve(PTR(D(2)));
	struct stat sa, sb;

	if (!a || !b) {
		RET(DOSFALSE);
	} else if (!lstat(b, &sb) && (lstat(a, &sa) || sa.st_ino != sb.st_ino ||
				      sa.st_dev != sb.st_dev)) {
		dos_set_ioerr(ERROR_OBJECT_EXISTS);
		RET(DOSFALSE);
	} else if (rename(a, b)) {
		dos_set_ioerr(dos_errno(errno));
		RET(DOSFALSE);
	} else
		RET(DOSTRUE);
	free(a);
	free(b);
}

static void d_SetProtection(struct frame *f)
{
	char *p = resolve(PTR(D(1)));
	struct stat st;
	uint32_t m = D(2);
	mode_t mode;

	if (!p || stat(p, &st)) {
		if (p)
			dos_set_ioerr(dos_errno(errno));
		free(p);
		RET(DOSFALSE);
		return;
	}
	mode = st.st_mode & 07077;
	if (!(m & FIBF_READ))
		mode |= S_IRUSR;
	if (!(m & FIBF_WRITE))
		mode |= S_IWUSR;
	if (!(m & FIBF_EXECUTE))
		mode |= S_IXUSR;
	if (chmod(p, mode)) {
		dos_set_ioerr(dos_errno(errno));
		RET(DOSFALSE);
	} else
		RET(DOSTRUE);
	free(p);
}

static void d_SetComment(struct frame *f)
{
	char *p = resolve(PTR(D(1)));
	struct stat st;

	if (!p || lstat(p, &st)) {
		if (p)
			dos_set_ioerr(dos_errno(errno));
		RET(DOSFALSE);
	} else
		RET(DOSTRUE);           /* Linux has no comments: accepted */
	free(p);
}

static void d_SetFileDate(struct frame *f)
{
	char *p = resolve(PTR(D(1)));
	struct timeval tv[2];

	if (!p) {
		RET(DOSFALSE);
		return;
	}
	tv[0].tv_sec = tv[1].tv_sec = date_to_unix(PTR(D(2)));
	tv[0].tv_usec = tv[1].tv_usec = 0;
	if (utimes(p, tv)) {
		dos_set_ioerr(dos_errno(errno));
		RET(DOSFALSE);
	} else
		RET(DOSTRUE);
	free(p);
}

static void d_SameLock(struct frame *f)
{
	struct stat a, b;

	if (stat(lock_path(D(1)), &a) || stat(lock_path(D(2)), &b))
		RET(-1);
	else if (a.st_dev != b.st_dev)
		RET(-1);                        /* LOCK_DIFFERENT */
	else
		RET(a.st_ino == b.st_ino ? 0 : 1);      /* SAME, SAME_VOLUME */
}

static void d_SameDevice(struct frame *f)
{
	struct stat a, b;

	RET(!stat(lock_path(D(1)), &a) && !stat(lock_path(D(2)), &b) &&
	    a.st_dev == b.st_dev ? DOSTRUE : DOSFALSE);
}

static void d_MakeLink(struct frame *f)
{
	char *p = resolve(PTR(D(1)));
	int r;

	if (!p) {
		RET(DOSFALSE);
		return;
	}
	if (D(3)) {                     /* soft: dest is a path string */
		char *t = path_to_linux(PTR(D(2)), base());
		r = t ? symlink(t, p) : -1;
		free(t);
	} else
		r = link(lock_path(D(2)), p);
	if (r) {
		dos_set_ioerr(dos_errno(errno));
		RET(DOSFALSE);
	} else
		RET(DOSTRUE);
	free(p);
}

static void d_ReadLink(struct frame *f)
{
	char full[PATH_MAX], *p;
	ssize_t n;

	snprintf(full, sizeof(full), "%s/%s", lock_path(D(2)),
		 (char *)PTR(D(3)));
	n = readlink(full, PTR(D(4)), D(5) ? D(5) - 1 : 0);
	if (n < 0) {
		dos_set_ioerr(dos_errno(errno));
		RET(DOSFALSE);
		return;
	}
	p = PTR(D(4));
	p[n] = 0;
	RET(DOSTRUE);
}

static void d_True(struct frame *f) { RET(DOSTRUE); }

/* ---- the DOS list: devices, volumes and directory assigns ---- */

/*
 * Built once: a device per Linux volume (LNX0..), each with its own port
 * that DoPkt() recognises, the volumes themselves (mounted on those
 * ports), then the directory assigns.
 */
static const char *vol_names[] = { "SYS", "RAM", "ROOT", NULL };
static struct MsgPort *vol_port[3];
static char *vol_path[3];

static struct dlent *dl_add(const char *name, int32_t type)
{
	struct dlent *e = &dl_head[dl_count];

	*(int32_t *)(e->node + 4) = type;
	e->name[0] = strlen(name);
	memcpy(e->name + 1, name, e->name[0]);
	*(BPTR *)(e->node + 40) = MKBADDR(e->name);
	if (dl_count)
		*(BPTR *)(dl_head[dl_count - 1].node) = MKBADDR(e->node);
	dl_count++;
	return e;
}

static void dl_build(void)
{
	static const char *names[] = {
		"HOME", "C", "S", "L", "LIBS", "DEVS", "FONTS", "LOCALE",
		"ENV", "ENVARC", "T", "PROGDIR", NULL
	};
	char a[40], *lp;
	int i;

	if (dl_head)
		return;
	dl_head = amem_alloc(sizeof(struct dlent) * 64);
	for (i = 0; vol_names[i]; i++) {
		struct dlent *e;
		snprintf(a, sizeof(a), "%s:", vol_names[i]);
		vol_path[i] = path_to_linux(a, "/");
		vol_port[i] = amem_alloc(sizeof(struct MsgPort));
		vol_port[i]->mp_Node.ln_Type = NT_MSGPORT;
		vol_port[i]->mp_Flags = 2;              /* PA_IGNORE */
		exec_newlist(&vol_port[i]->mp_MsgList);
		snprintf(a, sizeof(a), "LNX%d", i);
		e = dl_add(a, 0);                       /* DLT_DEVICE */
		*(APTR *)(e->node + 8) = APTR_OF(vol_port[i]);
	}
	for (i = 0; vol_names[i]; i++) {
		struct dlent *e = dl_add(vol_names[i], 2);      /* DLT_VOLUME */
		*(APTR *)(e->node + 8) = APTR_OF(vol_port[i]);
		*(int32_t *)(e->node + 32) = 0x444f5300;        /* ID_DOS_DISK */
	}
	for (i = 0; names[i]; i++) {
		struct dlent *e;
		snprintf(a, sizeof(a), "%s:", names[i]);
		lp = path_to_linux(a, "/");
		if (!lp)
			continue;
		e = dl_add(names[i], 1);                /* DLT_DIRECTORY */
		*(BPTR *)(e->node + 12) = lock_new(lp, SHARED_LOCK);
		free(lp);
	}
}

/* the Linux directory behind a device port, for DoPkt() */
const char *port_path(APTR port)
{
	dl_build();
	for (int i = 0; vol_names[i]; i++)
		if (APTR_OF(vol_port[i]) == port)
			return vol_path[i];
	return NULL;
}

static uint32_t dl_handle;              /* what LockDosList returns */

static void d_LockDosList(struct frame *f)
{
	dl_build();
	RET(APTR_OF(&dl_handle));
}

static void d_NextDosEntry(struct frame *f)
{
	uint8_t *e = PTR(D(1));
	uint32_t fl = D(2);
	int i;

	dl_build();
	i = e == (uint8_t *)&dl_handle ? 0 :
	    (int)(((struct dlent *)e - dl_head) + 1);
	for (; i < dl_count; i++) {
		int32_t t = *(int32_t *)dl_head[i].node;
		t = *(int32_t *)(dl_head[i].node + 4);
		if ((t == 0 && (fl & 4)) || (t == 2 && (fl & 8)) ||
		    ((t == 1 || t == 3 || t == 4) && (fl & 16))) {
			RET(dl_head[i].node);
			return;
		}
	}
	RET(0);
}

static void d_FindDosEntry(struct frame *f)
{
	const char *n = PTR(D(2));
	size_t l = strlen(n);
	int i;

	dl_build();
	if (l && n[l - 1] == ':')
		l--;
	i = (uint8_t *)PTR(D(1)) == (uint8_t *)&dl_handle ? 0 :
	    (int)(((struct dlent *)PTR(D(1)) - dl_head) + 1);
	for (; i < dl_count; i++)
		if (dl_head[i].name[0] == l &&
		    !strncasecmp((char *)dl_head[i].name + 1, n, l)) {
			RET(dl_head[i].node);
			return;
		}
	RET(0);
}

static void d_UnLockDosList(struct frame *f) { (void)f; }

static void d_GetDeviceProc(struct frame *f)
{
	const char *n = PTR(D(1));
	const char *c = strchr(n, ':');
	uint32_t *dp, *old = PTR(D(2));
	char dev[256], *lp;

	if (old) {
		/* the next directory of a multi-directory assign: an assign
		   has only one here */
		if (old[2] & 1)
			lock_free(old[1]);
		amem_free(old);
		dos_set_ioerr(ERROR_NO_MORE_ENTRIES);
		RET(0);
		return;
	}
	dp = amem_alloc(16);

	dp[0] = dos_fs_port();          /* packets to it are answered */
	if (c) {
		snprintf(dev, sizeof(dev), "%.*s:", (int)(c - n), n);
		lp = path_to_linux(dev, "/");
		if (!lp) {
			amem_free(dp);
			dos_set_ioerr(ERROR_DEVICE_NOT_MOUNTED);
			RET(0);
			return;
		}
		dp[1] = lock_new(lp, SHARED_LOCK);
		dp[2] = 1;              /* DVPF_UNLOCK */
		free(lp);
	} else
		dp[1] = me->pr_CurrentDir;      /* a relative name: the current
						   directory, which stays */
	RET(dp);
}

static void d_FreeDeviceProc(struct frame *f)
{
	uint32_t *dp = PTR(D(1));

	if (!dp)
		return;
	if (dp[2] & 1)
		lock_free(dp[1]);
	amem_free(dp);
}

static void d_DeviceProc(struct frame *f)
{
	RET(dos_fs_port());
	dos_set_ioerr(0);
}

/*
 * Assigns made while the program runs (C:Assign, a script's assigns):
 * path.c's table, and a DOS list entry so that they are listed.  One
 * directory each: AssignAdd() only makes a new one.
 */
static int dl_find_assign(const char *name)
{
	size_t l = strlen(name);
	int i;

	if (l && name[l - 1] == ':')
		l--;
	dl_build();
	for (i = 0; i < dl_count; i++) {
		int32_t t = *(int32_t *)(dl_head[i].node + 4);
		if ((t == 1 || t == 3 || t == 4) && dl_head[i].name[0] == l &&
		    !strncasecmp((char *)dl_head[i].name + 1, name, l))
			return i;
	}
	return -1;
}

static void dl_remove(int i)
{
	*(int32_t *)(dl_head[i].node + 4) = -1;         /* skipped */
	dl_head[i].name[0] = 0;
	if (i)
		*(BPTR *)dl_head[i - 1].node = *(BPTR *)dl_head[i].node;
}

static struct dlent *dl_assign(const char *name, int32_t type)
{
	char n[32];
	size_t l = strlen(name);
	int i = dl_find_assign(name);

	if (l && name[l - 1] == ':')
		l--;
	if (l >= sizeof(n))
		l = sizeof(n) - 1;
	memcpy(n, name, l);
	n[l] = 0;
	if (i >= 0) {
		BPTR old = *(BPTR *)(dl_head[i].node + 12);
		if (old)
			lock_free(old);
		*(BPTR *)(dl_head[i].node + 12) = 0;
		*(int32_t *)(dl_head[i].node + 4) = type;
		return &dl_head[i];
	}
	if (dl_count == 64)
		return NULL;
	return dl_add(n, type);
}

static void d_AssignLock(struct frame *f)
{
	const char *name = PTR(D(1));
	struct dlent *e;
	int i;

	if (!D(2)) {                            /* remove it */
		i = dl_find_assign(name);
		if (!path_unassign(name) && i < 0) {
			dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
			RET(DOSFALSE);
			return;
		}
		if (i >= 0)
			dl_remove(i);
		RET(DOSTRUE);
		return;
	}
	path_assign_dir(name, lock_path(D(2)));
	e = dl_assign(name, 1);                 /* DLT_DIRECTORY */
	if (e)
		*(BPTR *)(e->node + 12) = D(2);         /* the lock is ours now */
	RET(DOSTRUE);
}

/* AssignLate and AssignPath: the target stays a path */
static void assign_path(struct frame *f, int32_t type)
{
	const char *path = PTR(D(2));
	struct dlent *e;
	char *copy;

	path_assign_dir(PTR(D(1)), path);
	e = dl_assign(PTR(D(1)), type);
	if (e) {
		copy = amem_alloc(strlen(path) + 1);
		strcpy(copy, path);
		*(APTR *)(e->node + 16) = APTR_OF(copy);   /* dol_AssignName */
	}
	RET(DOSTRUE);
}

static void d_AssignLate(struct frame *f) { assign_path(f, 3); }
static void d_AssignPath(struct frame *f) { assign_path(f, 4); }

static void d_AssignAdd(struct frame *f)
{
	if (dl_find_assign(PTR(D(1))) >= 0) {
		dos_set_ioerr(ERROR_OBJECT_EXISTS);
		RET(DOSFALSE);
		return;
	}
	d_AssignLock(f);
}

static void d_RemAssignList(struct frame *f)
{
	int i = dl_find_assign(PTR(D(1)));
	BPTR l;

	if (i < 0) {
		RET(DOSFALSE);
		return;
	}
	l = *(BPTR *)(dl_head[i].node + 12);
	if (l && D(2) && strcmp(lock_path(l), lock_path(D(2)))) {
		RET(DOSFALSE);
		return;
	}
	path_unassign(PTR(D(1)));
	dl_remove(i);
	RET(DOSTRUE);
}

/*
 * DOS list entries a program makes (C:Mount): built and freed, never
 * added, because a device needs a handler to run it and there are none
 * here.  AddDosEntry() refuses with ERROR_NOT_IMPLEMENTED.
 */
static void d_MakeDosEntry(struct frame *f)
{
	const char *name = PTR(D(1));
	size_t n = strlen(name);
	uint8_t *dl = amem_alloc(44 + n + 2);

	if (!dl) {
		dos_set_ioerr(ERROR_NO_FREE_STORE);
		RET(0);
		return;
	}
	if (n > 255)
		n = 255;
	*(int32_t *)(dl + 4) = D(2);                    /* dol_Type */
	dl[44] = n;
	memcpy(dl + 45, name, n);
	*(BPTR *)(dl + 40) = MKBADDR(dl + 44);          /* dol_Name */
	RET(dl);
}

static void d_FreeDosEntry(struct frame *f)
{
	if (D(1))
		amem_free(PTR(D(1)));
}

static void d_AddDosEntry(struct frame *f)
{
	dos_set_ioerr(ERROR_NOT_IMPLEMENTED);
	RET(DOSFALSE);
}

/* ---- MatchFirst / MatchNext ---- */

struct mlevel {
	DIR *d;
	char *lpath;            /* Linux directory */
	char *apath;            /* its Amiga prefix, for ap_Buf */
	struct AChain *ac;
	char pat[256];
	char dirname[108];      /* the entry that led here */
};

struct mstate {
	struct AnchorPath *ap;
	struct mlevel lv[32];
	int depth;
	int single;             /* not wild: one object, already returned */
	char topdir[256];       /* that object, while DODIR walks it */
	int lastdir;            /* the last entry returned was a directory */
	char lastname[256];
	struct mstate *next;
};

static struct mstate *matches;

static struct mstate *mfind(struct AnchorPath *ap)
{
	struct mstate *m;

	for (m = matches; m; m = m->next)
		if (m->ap == ap)
			return m;
	return NULL;
}

static struct AChain *achain_new(const char *lpath, struct AChain *parent)
{
	struct AChain *ac = amem_alloc(sizeof(*ac) + 256);

	ac->an_Lock = lock_new(lpath, SHARED_LOCK);
	ac->an_Parent = APTR_OF(parent);
	if (parent)
		parent->an_Child = APTR_OF(ac);
	return ac;
}

static void achain_free(struct AChain *ac)
{
	lock_free(ac->an_Lock);
	amem_free(ac);
}

static void set_buf(struct AnchorPath *ap, const char *apath, const char *name)
{
	char tmp[1024];
	size_t l;

	if (ap->ap_Strlen <= 0)
		return;
	snprintf(tmp, sizeof(tmp), "%s", apath);
	l = strlen(tmp);
	if (l && tmp[l - 1] != ':' && tmp[l - 1] != '/' && *name)
		tmp[l++] = '/', tmp[l] = 0;
	snprintf(tmp + l, sizeof(tmp) - l, "%s", name);
	snprintf(ap->ap_Buf, ap->ap_Strlen, "%s", tmp);
}

static int32_t push_level(struct mstate *m, const char *lpath,
			  const char *apath, const char *pat, const char *dn)
{
	struct mlevel *lv;

	if (m->depth >= 31)
		return ERROR_TOO_MANY_LEVELS;
	lv = &m->lv[m->depth];
	lv->d = opendir(lpath);
	if (!lv->d)
		return dos_errno(errno);
	lv->lpath = strdup(lpath);
	lv->apath = strdup(apath);
	snprintf(lv->pat, sizeof(lv->pat), "%s", pat);
	snprintf(lv->dirname, sizeof(lv->dirname), "%s", dn ? dn : "");
	lv->ac = achain_new(lpath, m->depth ? m->lv[m->depth - 1].ac : NULL);
	if (!m->depth)
		m->ap->ap_Base = APTR_OF(lv->ac);
	m->ap->ap_Last = APTR_OF(lv->ac);
	m->depth++;
	return 0;
}

static void pop_level(struct mstate *m)
{
	struct mlevel *lv = &m->lv[--m->depth];

	closedir(lv->d);
	free(lv->lpath);
	free(lv->apath);
	achain_free(lv->ac);
	if (m->depth) {
		m->lv[m->depth - 1].ac->an_Child = 0;
		m->ap->ap_Last = APTR_OF(m->lv[m->depth - 1].ac);
	} else
		m->ap->ap_Base = m->ap->ap_Last = 0;
}

static int32_t match_step(struct mstate *m)
{
	struct AnchorPath *ap = m->ap;
	struct dirent *e;
	char full[PATH_MAX];
	struct stat st;

	for (;;) {
		struct mlevel *lv;
		if (!m->depth)
			return ERROR_NO_MORE_ENTRIES;
		lv = &m->lv[m->depth - 1];
		while ((e = next_entry(lv->d))) {
			if (!pat_matchraw(lv->pat, e->d_name, 1))
				continue;
			snprintf(full, sizeof(full), "%s/%s", lv->lpath,
				 e->d_name);
			if (stat_entry(full, &st))
				continue;
			fill_fib(&ap->ap_Info, full, e->d_name, &st, 0);
			set_buf(ap, lv->apath, e->d_name);
			ap->ap_Flags &= ~APF_DIDDIR;
			m->lastdir = S_ISDIR(st.st_mode);
			snprintf(m->lastname, sizeof(m->lastname), "%s",
				 e->d_name);
			return 0;
		}
		/* level done: report the directory again, as DIDDIR */
		if (m->depth == 1 && m->topdir[0]) {
			/* the single object we were asked to walk, as DIDDIR */
			char *lp = resolve(m->topdir), *s;
			pop_level(m);
			if (!lp)
				return ERROR_NO_MORE_ENTRIES;
			examine_path(lp, &ap->ap_Info);
			set_buf(ap, m->topdir, "");
			s = strrchr(lp, '/');
			if (s && s != lp)
				*s = 0;
			else if (s)
				s[1] = 0;
			m->lv[0].ac = achain_new(lp, NULL);
			ap->ap_Base = ap->ap_Last = APTR_OF(m->lv[0].ac);
			free(lp);
			ap->ap_Flags |= APF_DIDDIR;
			m->topdir[0] = 0;
			m->single = 1;
			m->lastdir = 0;
			return 0;
		} else if (m->depth == 1) {
			return ERROR_NO_MORE_ENTRIES;
		} else {
			char dn[108], parent_l[PATH_MAX], parent_a[1024];
			snprintf(dn, sizeof(dn), "%s", lv->dirname);
			pop_level(m);
			lv = &m->lv[m->depth - 1];
			snprintf(parent_l, sizeof(parent_l), "%s/%s", lv->lpath, dn);
			snprintf(parent_a, sizeof(parent_a), "%s", lv->apath);
			if (stat_entry(parent_l, &st))
				continue;
			fill_fib(&ap->ap_Info, parent_l, dn, &st, 0);
			set_buf(ap, parent_a, dn);
			ap->ap_Flags |= APF_DIDDIR;
			m->lastdir = 0;
			return 0;
		}
	}
}

static int32_t match_first(const char *pat, struct AnchorPath *ap)
{
	struct mstate *m = calloc(1, sizeof(*m));
	const char *slash = strrchr(pat, '/'), *colon = strchr(pat, ':');
	const char *fpart = slash ? slash + 1 : colon ? colon + 1 : pat;
	char dirpart[1024], parsed[600], *ldir;
	int32_t wild, err;

	m->ap = ap;
	m->next = matches;
	matches = m;
	ap->ap_Flags &= ~(APF_ITSWILD | APF_DIDDIR);
	ap->ap_FoundBreak = 0;
	snprintf(dirpart, sizeof(dirpart), "%.*s", (int)(fpart - pat), pat);
	wild = pat_parse(fpart, parsed, sizeof(parsed), 1);
	if (wild < 0)
		return ERROR_BAD_TEMPLATE;
	if (!strcmp(pat, "*") || !strcasecmp(pat, "CONSOLE:") ||
	    !strcasecmp(pat, "NIL:")) {
		/* streams, not files: let the program Open() them (Type *) */
		memset(&ap->ap_Info, 0, sizeof(ap->ap_Info));
		ap->ap_Info.fib_DirEntryType = ap->ap_Info.fib_EntryType = ST_FILE;
		snprintf(ap->ap_Info.fib_FileName, 108, "%s", pat);
		set_buf(ap, pat, "");
		m->lv[0].ac = achain_new(base(), NULL);
		ap->ap_Base = ap->ap_Last = APTR_OF(m->lv[0].ac);
		m->single = 1;
		return 0;
	}
	if (!wild) {
		/* one object: Lock and Examine it */
		char *lp = resolve(pat), *s;
		struct stat st;
		if (!lp)
			return ERROR_DEVICE_NOT_MOUNTED;
		if (stat(lp, &st) && lstat(lp, &st)) {
			free(lp);
			return dos_errno(errno);
		}
		examine_path(lp, &ap->ap_Info);
		set_buf(ap, pat, "");
		s = strrchr(lp, '/');
		if (s && s != lp) {
			*s = 0;
		} else if (s)
			s[1] = 0;
		m->lv[0].ac = achain_new(lp, NULL);
		ap->ap_Base = ap->ap_Last = APTR_OF(m->lv[0].ac);
		free(lp);
		m->single = 1;
		m->lastdir = S_ISDIR(st.st_mode);
		snprintf(m->lastname, sizeof(m->lastname), "%s", pat);
		return 0;
	}
	ap->ap_Flags |= APF_ITSWILD;
	ldir = resolve(dirpart);
	if (!ldir)
		return ERROR_DEVICE_NOT_MOUNTED;
	err = push_level(m, ldir, dirpart, fpart, NULL);
	free(ldir);
	if (err)
		return err;
	return match_step(m);
}

static int32_t match_next(struct AnchorPath *ap)
{
	struct mstate *m = mfind(ap);
	int32_t err;

	if (!m)
		return ERROR_NO_MORE_ENTRIES;
	exec_pending_break();
	if (ap->ap_BreakBits & me->pr_Task.tc_SigRecvd) {
		ap->ap_FoundBreak = ap->ap_BreakBits & me->pr_Task.tc_SigRecvd;
		me->pr_Task.tc_SigRecvd &= ~ap->ap_FoundBreak;
		return ERROR_BREAK;
	}
	if ((ap->ap_Flags & APF_DODIR) && m->lastdir) {
		char full[PATH_MAX], apath[1024];
		ap->ap_Flags &= ~APF_DODIR;
		m->lastdir = 0;
		if (m->single) {
			char *lp = resolve(m->lastname);
			m->single = 0;
			if (m->lv[0].ac) {
				achain_free(m->lv[0].ac);
				m->lv[0].ac = NULL;
			}
			if (!lp)
				return ERROR_NO_MORE_ENTRIES;
			err = push_level(m, lp, m->lastname, "#?", NULL);
			free(lp);
			if (err)
				return err;
			snprintf(m->topdir, sizeof(m->topdir), "%s",
				 m->lastname);
			return match_step(m);
		}
		{
			struct mlevel *lv = &m->lv[m->depth - 1];
			size_t l;
			snprintf(full, sizeof(full), "%s/%s", lv->lpath,
				 m->lastname);
			snprintf(apath, sizeof(apath), "%s", lv->apath);
			l = strlen(apath);
			if (l && apath[l - 1] != ':' && apath[l - 1] != '/')
				apath[l++] = '/', apath[l] = 0;
			snprintf(apath + l, sizeof(apath) - l, "%s",
				 m->lastname);
			err = push_level(m, full, apath, lv->pat, m->lastname);
			if (err)
				return err;
		}
	}
	if (m->single)
		return ERROR_NO_MORE_ENTRIES;
	return match_step(m);
}

static void match_end(struct AnchorPath *ap)
{
	struct mstate **pp, *m;

	for (pp = &matches; (m = *pp); pp = &m->next)
		if (m->ap == ap) {
			while (m->depth)
				pop_level(m);
			if (m->lv[0].ac && m->single)
				achain_free(m->lv[0].ac);
			*pp = m->next;
			free(m);
			break;
		}
	ap->ap_Base = ap->ap_Last = 0;
}

static void d_MatchFirst(struct frame *f)
{
	struct AnchorPath *ap = PTR(D(2));
	int32_t e;

	if (mfind(ap))
		match_end(ap);
	e = match_first(PTR(D(1)), ap);
	dos_set_ioerr(e);
	RET(e);
}

static void d_MatchNext(struct frame *f)
{
	int32_t e = match_next(PTR(D(1)));

	dos_set_ioerr(e);
	RET(e);
}

static void d_MatchEnd(struct frame *f) { match_end(PTR(D(1))); }

const struct libimpl dos_impl_lock[] = {
	{ "Lock", d_Lock }, { "UnLock", d_UnLock }, { "DupLock", d_DupLock },
	{ "CurrentDir", d_CurrentDir }, { "ParentDir", d_ParentDir },
	{ "Examine", d_Examine }, { "ExNext", d_ExNext },
	{ "ExamineFH", d_ExamineFH },
	{ "ExAll", d_ExAll }, { "ExAllEnd", d_ExAllEnd },
	{ "Info", d_Info },
	{ "NameFromLock", d_NameFromLock }, { "NameFromFH", d_NameFromFH },
	{ "DupLockFromFH", d_DupLockFromFH }, { "ParentOfFH", d_ParentOfFH },
	{ "OpenFromLock", d_OpenFromLock },
	{ "CreateDir", d_CreateDir }, { "DeleteFile", d_DeleteFile },
	{ "Rename", d_Rename }, { "SetProtection", d_SetProtection },
	{ "SetComment", d_SetComment }, { "SetFileDate", d_SetFileDate },
	{ "SetOwner", d_True },
	{ "SameLock", d_SameLock }, { "SameDevice", d_SameDevice },
	{ "MakeLink", d_MakeLink }, { "ReadLink", d_ReadLink },
	{ "ChangeMode", d_True },
	{ "LockDosList", d_LockDosList },
	{ "AttemptLockDosList", d_LockDosList },
	{ "MakeDosEntry", d_MakeDosEntry }, { "FreeDosEntry", d_FreeDosEntry },
	{ "AddDosEntry", d_AddDosEntry }, { "RemDosEntry", d_AddDosEntry },
	{ "UnLockDosList", d_UnLockDosList },
	{ "NextDosEntry", d_NextDosEntry }, { "FindDosEntry", d_FindDosEntry },
	{ "GetDeviceProc", d_GetDeviceProc },
	{ "FreeDeviceProc", d_FreeDeviceProc },
	{ "DeviceProc", d_DeviceProc },
	{ "AssignLock", d_AssignLock }, { "AssignLate", d_AssignLate },
	{ "AssignPath", d_AssignPath }, { "AssignAdd", d_AssignAdd },
	{ "RemAssignList", d_RemAssignList },
	{ "MatchFirst", d_MatchFirst }, { "MatchNext", d_MatchNext },
	{ "MatchEnd", d_MatchEnd },
	{ NULL, NULL }
};
