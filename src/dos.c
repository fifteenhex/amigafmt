/*
 * dos.library: the process, file handles, buffered I/O, errors, misc.
 * Locks, directories and pattern matching are in lock.c, ReadArgs in
 * readargs.c.
 *
 * A FileHandle is the real structure, 4-byte aligned so BPTRs work; its
 * fh_Args points at our struct afh, which holds the Linux descriptors
 * (separate read and write ones, because "*" is the console both ways)
 * and the buffers behind FGetC/FPutC/FPuts/VFPrintf.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include "af.h"
#include "dos_int.h"

void exec_putmsg(struct MsgPort *p, struct Message *m);

struct DosLibrary *DOSBase;
struct Process *me;
char *dos_args;
uint32_t dos_arglen;
static struct MsgPort *fs_port;         /* stands in for handler ports */
static struct MsgPort *con_port;        /* the console handler's */

#define MAXFH 256
static struct afh *open_fhs[MAXFH];

/* ---- errors ---- */

int32_t dos_errno(int e)
{
	switch (e) {
	case 0: return 0;
	case ENOENT: return ERROR_OBJECT_NOT_FOUND;
	case EEXIST: return ERROR_OBJECT_EXISTS;
	case ENOTDIR: return ERROR_DIR_NOT_FOUND;
	case EISDIR: return ERROR_OBJECT_WRONG_TYPE;
	case EACCES: case EPERM: return ERROR_WRITE_PROTECTED;
	case EROFS: return ERROR_DISK_WRITE_PROTECTED;
	case ENOSPC: return ERROR_DISK_FULL;
	case ENOTEMPTY: return ERROR_DIRECTORY_NOT_EMPTY;
	case EBUSY: case ETXTBSY: return ERROR_OBJECT_IN_USE;
	case ENOMEM: return ERROR_NO_FREE_STORE;
	case EXDEV: return ERROR_RENAME_ACROSS_DEVICES;
	case ENAMETOOLONG: return ERROR_INVALID_COMPONENT_NAME;
	case ELOOP: return ERROR_TOO_MANY_LEVELS;
	case ESPIPE: return ERROR_SEEK_ERROR;
	case EFBIG: return ERROR_OBJECT_TOO_LARGE;
	default: return ERROR_ACTION_NOT_KNOWN;
	}
}

void dos_set_ioerr(int32_t e)
{
	if (me)
		me->pr_Result2 = e;
}

/* AmigaOS 3.1's English fault texts */
static const struct { int16_t code; const char *msg; } faults[] = {
	{ 103, "not enough memory available" },
	{ 105, "process table full" },
	{ 114, "bad template" },
	{ 115, "bad number" },
	{ 116, "required argument missing" },
	{ 117, "value after keyword missing" },
	{ 118, "wrong number of arguments" },
	{ 119, "unmatched quotes" },
	{ 120, "argument line invalid or too long" },
	{ 121, "file is not executable" },
	{ 122, "invalid resident library" },
	{ 202, "object is in use" },
	{ 203, "object already exists" },
	{ 204, "directory not found" },
	{ 205, "object not found" },
	{ 206, "invalid window description" },
	{ 207, "object too large" },
	{ 209, "packet request type unknown" },
	{ 210, "object name invalid" },
	{ 211, "invalid object lock" },
	{ 212, "object is not of required type" },
	{ 213, "disk not validated" },
	{ 214, "disk is write-protected" },
	{ 215, "rename across devices attempted" },
	{ 216, "directory not empty" },
	{ 217, "too many levels" },
	{ 218, "device (or volume) is not mounted" },
	{ 219, "seek failure" },
	{ 220, "comment is too long" },
	{ 221, "disk is full" },
	{ 222, "object is protected from deletion" },
	{ 223, "file is write protected" },
	{ 224, "file is read protected" },
	{ 225, "not a valid DOS disk" },
	{ 226, "no disk in drive" },
	{ 232, "no more entries in directory" },
	{ 233, "object is soft link" },
	{ 234, "object is linked" },
	{ 235, "bad loadfile hunk" },
	{ 236, "function not implemented" },
	{ 240, "record not locked" },
	{ 241, "lock collision" },
	{ 242, "lock timeout" },
	{ 243, "unlock error" },
	{ 303, "buffer overflow" },
	{ 304, "***Break" },
	{ 305, "file not executable" },
};

static int fault_text(int32_t code, const char *hdr, char *buf, int32_t len)
{
	const char *m = NULL;
	char tmp[300], num[32];
	size_t i;

	for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++)
		if (faults[i].code == code)
			m = faults[i].msg;
	if (!m) {
		snprintf(num, sizeof(num), "Error %d", (int)code);
		m = num;
	}
	if (hdr && *hdr)
		snprintf(tmp, sizeof(tmp), "%s: %s", hdr, m);
	else
		snprintf(tmp, sizeof(tmp), "%s", m);
	if (len <= 0)
		return 0;
	snprintf(buf, len, "%s", tmp);
	return strlen(buf);
}

/* ---- file handles ---- */

struct afh *fh_priv(BPTR b)
{
	struct FileHandle *fh = BADDR(b);

	if (!b)
		return NULL;
	return (struct afh *)PTR(fh->fh_Args);
}

BPTR fh_new(int rfd, int wfd, int keep, const char *path)
{
	struct FileHandle *fh = amem_alloc(sizeof(*fh));
	struct afh *a = calloc(1, sizeof(*a));
	int i;

	a->rfd = rfd;
	a->wfd = wfd;
	a->keep = keep;
	a->unget = -1;
	a->path = path ? strdup(path) : NULL;
	a->interactive = isatty(rfd >= 0 ? rfd : wfd);
	a->tty_out = wfd >= 0 && isatty(wfd) && !getenv("AMIGAFMT_RAWCSI");
	{
		struct stat st;
		a->seekable = rfd == wfd && rfd >= 0 && !fstat(rfd, &st) &&
			      S_ISREG(st.st_mode);
	}
	fh->fh_Args = (int32_t)(uintptr_t)a;
	fh->fh_Type = APTR_OF(a->interactive ? con_port : fs_port);
	fh->fh_Port = a->interactive ? (APTR)DOSTRUE : 0;
	fh->fh_Pos = fh->fh_End = -1;
	a->fh = fh;
	for (i = 0; i < MAXFH; i++)
		if (!open_fhs[i]) {
			open_fhs[i] = a;
			break;
		}
	return MKBADDR(fh);
}

/* ---- the console ---- */

/* what the console "types" back (window bounds reports), read before fd 0 */
static char con_reply[64];
static int con_nreply;

static int write_all(int fd, const char *p, size_t len)
{
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

/*
 * Output to a terminal: the Amiga console's 8-bit CSI (0x9B) becomes
 * ESC [, its cursor on/off become the xterm ones, and a Window Status
 * Request (CSI 0 SPACE q, how C:Dir learns the width) is answered from
 * TIOCGWINSZ as the console would: CSI 1;1;rows;cols SPACE r.
 */
static int con_write(int fd, const char *p, size_t len)
{
	char out[8192];
	size_t n = 0, i;

	for (i = 0; i < len; i++) {
		if (n > sizeof(out) - 16) {
			if (write_all(fd, out, n))
				return -1;
			n = 0;
		}
		if ((unsigned char)p[i] != 0x9b) {
			out[n++] = p[i];
			continue;
		}
		if (len - i >= 4 && !memcmp(p + i + 1, "0 q", 3)) {
			struct winsize ws;
			int rows = 24, cols = 80;
			if (!ioctl(fd, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col)
				rows = ws.ws_row, cols = ws.ws_col;
			con_nreply = snprintf(con_reply, sizeof(con_reply),
					      "\x9b" "1;1;%d;%d r", rows, cols);
			i += 3;
		} else if (len - i >= 4 && !memcmp(p + i + 1, "0 p", 3)) {
			memcpy(out + n, "\033[?25l", 6);
			n += 6;
			i += 3;
		} else if (len - i >= 3 && !memcmp(p + i + 1, " p", 2)) {
			memcpy(out + n, "\033[?25h", 6);
			n += 6;
			i += 2;
		} else {
			out[n++] = 033;
			out[n++] = '[';
		}
	}
	return write_all(fd, out, n);
}

static ssize_t fd_read(int fd, void *buf, size_t len)
{
	if (fd == 0 && con_nreply) {
		size_t n = (size_t)con_nreply < len ? (size_t)con_nreply : len;
		memcpy(buf, con_reply, n);
		memmove(con_reply, con_reply + n, con_nreply - n);
		con_nreply -= n;
		return n;
	}
	return read(fd, buf, len);
}

static int fd_write(struct afh *a, const char *p, size_t len)
{
	return a->tty_out ? con_write(a->wfd, p, len) : write_all(a->wfd, p, len);
}

int fh_flush(struct afh *a)
{
	int r = 0;

	if (a->wlen)
		r = fd_write(a, (char *)a->wbuf, a->wlen);
	a->wlen = 0;
	return r;
}

void dos_flush_all(void)
{
	for (int i = 0; i < MAXFH; i++)
		if (open_fhs[i] && open_fhs[i]->wlen)
			fh_flush(open_fhs[i]);
}

/* discard read-ahead, moving the file position back to what was used */
static void fh_unread(struct afh *a)
{
	int left = a->rlen - a->rpos + (a->unget >= 0);

	if (!a->seekable)
		return;
	if (left > 0)
		lseek(a->rfd, -left, SEEK_CUR);
	a->rpos = a->rlen = 0;
	a->unget = -1;
}

static int fh_fill(struct afh *a)
{
	ssize_t n;

	if (a->interactive || a->rfd == 0)
		dos_flush_all();
	do
		n = fd_read(a->rfd, a->rbuf, sizeof(a->rbuf));
	while (n < 0 && errno == EINTR);
	a->rpos = 0;
	a->rlen = n > 0 ? n : 0;
	return n;
}

int fh_getc(struct afh *a)
{
	int c;

	if (a->unget >= 0) {
		c = a->unget;
		a->unget = -1;
		return c;
	}
	if (a->rpos >= a->rlen && fh_fill(a) <= 0)
		return -1;
	c = a->rbuf[a->rpos++];
	a->last = c;
	return c;
}

int fh_putc(struct afh *a, int c)
{
	if (a->rlen && a->seekable)
		fh_unread(a);
	a->wbuf[a->wlen++] = c;
	if (a->wlen == sizeof(a->wbuf) || (c == '\n' && a->interactive))
		return fh_flush(a);
	return 0;
}

int fh_write(struct afh *a, const void *buf, int32_t len)
{
	if (a->rlen && a->seekable)
		fh_unread(a);
	if (a->wlen && fh_flush(a) < 0)
		return -1;
	if (fd_write(a, buf, len))
		return -1;
	return len;
}

int32_t fh_read(struct afh *a, void *buf, int32_t len)
{
	char *p = buf;
	int32_t done = 0;

	if (a->wlen)
		fh_flush(a);
	if (len > 0 && a->unget >= 0) {
		p[done++] = a->unget;
		a->unget = -1;
	}
	while (done < len && a->rpos < a->rlen)
		p[done++] = a->rbuf[a->rpos++];
	if (done < len && (a->interactive || a->rfd == 0))
		dos_flush_all();
	while (done < len) {
		ssize_t n = fd_read(a->rfd, p + done, len - done);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return done ? done : -1;
		}
		if (n == 0)
			break;
		done += n;
		if (a->interactive)
			break;          /* a console returns a line at a time */
	}
	return done;
}

static void fh_close(BPTR b)
{
	struct afh *a = fh_priv(b);
	int i;

	if (!a)
		return;
	fh_flush(a);
	if (!a->keep) {
		if (a->rfd >= 0)
			close(a->rfd);
		if (a->wfd >= 0 && a->wfd != a->rfd)
			close(a->wfd);
	}
	for (i = 0; i < MAXFH; i++)
		if (open_fhs[i] == a)
			open_fhs[i] = NULL;
	free(a->path);
	free(a);
	amem_free(BADDR(b));
}

static int is_dev(const char *name, const char *dev)
{
	size_t l = strlen(dev);
	return !strncasecmp(name, dev, l);
}

BPTR dos_open(const char *name, int32_t mode)
{
	char *lp;
	int fd, fl;
	struct stat st;
	BPTR b;

	if (!strcmp(name, "*") || !strcasecmp(name, "CONSOLE:") ||
	    is_dev(name, "CON:") || is_dev(name, "RAW:")) {
		int t = open("/dev/tty", O_RDWR);
		if (t >= 0 && (is_dev(name, "CON:") || is_dev(name, "RAW:")))
			return fh_new(t, t, 0, NULL);
		if (t >= 0)
			close(t);
		return fh_new(0, 1, 1, NULL);
	}
	if (!strcasecmp(name, "NIL:")) {
		fd = open("/dev/null", O_RDWR);
		b = fh_new(fd, fd, 0, NULL);
		((struct FileHandle *)BADDR(b))->fh_Type = 0;
		return b;
	}
	lp = path_to_linux(name, NULL);
	if (!lp) {
		dos_set_ioerr(ERROR_DEVICE_NOT_MOUNTED);
		return 0;
	}
	fl = mode == MODE_NEWFILE ? O_RDWR | O_CREAT | O_TRUNC :
	     mode == MODE_READWRITE ? O_RDWR | O_CREAT : O_RDWR;
	fd = open(lp, fl, 0666);
	if (fd < 0 && (errno == EACCES || errno == EROFS || errno == EISDIR) &&
	    mode == MODE_OLDFILE)
		fd = open(lp, O_RDONLY);
	if (fd < 0 && errno == EACCES && mode == MODE_NEWFILE)
		fd = open(lp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd >= 0 && !fstat(fd, &st) && S_ISDIR(st.st_mode)) {
		close(fd);
		errno = EISDIR;
		fd = -1;
	}
	if (fd < 0) {
		int e = errno;
		dos_set_ioerr(e == EACCES && mode == MODE_OLDFILE ?
			      ERROR_READ_PROTECTED : dos_errno(e));
		free(lp);
		return 0;
	}
	b = fh_new(fd, fd, 0, lp);
	free(lp);
	return b;
}

static void d_Open(struct frame *f)
{
	BPTR b = dos_open(PTR(D(1)), D(2));
	if (b)
		dos_set_ioerr(0);
	RET(b);
}

static void d_Close(struct frame *f)
{
	struct afh *a = fh_priv(D(1));

	if (!a) {
		RET(DOSTRUE);
		return;
	}
	if (a->keep) {
		/* the shell's own streams stay open; just flush them */
		fh_flush(a);
		RET(DOSTRUE);
		return;
	}
	fh_close(D(1));
	RET(DOSTRUE);
}

static void d_Read(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	int32_t n;

	if (!a || (int32_t)D(3) < 0) {
		RET(-1);
		return;
	}
	n = fh_read(a, PTR(D(2)), D(3));
	if (n < 0)
		dos_set_ioerr(dos_errno(errno));
	RET(n);
}

static void d_Write(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	int32_t n;

	if (!a || (int32_t)D(3) < 0) {
		RET(-1);
		return;
	}
	n = fh_write(a, PTR(D(2)), D(3));
	if (n < 0)
		dos_set_ioerr(dos_errno(errno));
	RET(n);
}

static void d_Input(struct frame *f) { RET(me->pr_CIS); }
static void d_Output(struct frame *f) { RET(me->pr_COS); }

static void d_SelectInput(struct frame *f)
{
	RET(me->pr_CIS);
	me->pr_CIS = D(1);
}

static void d_SelectOutput(struct frame *f)
{
	RET(me->pr_COS);
	me->pr_COS = D(1);
}

static void d_Seek(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	int32_t mode = D(3);
	off_t old, n;

	if (!a) {
		RET(-1);
		return;
	}
	fh_flush(a);
	fh_unread(a);
	old = lseek(a->rfd, 0, SEEK_CUR);
	if (old < 0) {
		dos_set_ioerr(ERROR_SEEK_ERROR);
		RET(-1);
		return;
	}
	n = lseek(a->rfd, (int32_t)D(2), mode == OFFSET_BEGINNING ? SEEK_SET :
		  mode == OFFSET_END ? SEEK_END : SEEK_CUR);
	if (n < 0) {
		dos_set_ioerr(ERROR_SEEK_ERROR);
		RET(-1);
		return;
	}
	RET(old);
}

static void d_SetFileSize(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	int32_t mode = D(3);
	off_t cur, end, n;

	if (!a) {
		RET(-1);
		return;
	}
	fh_flush(a);
	fh_unread(a);
	cur = lseek(a->rfd, 0, SEEK_CUR);
	end = lseek(a->rfd, 0, SEEK_END);
	lseek(a->rfd, cur, SEEK_SET);
	n = (int32_t)D(2) + (mode == OFFSET_BEGINNING ? 0 :
			     mode == OFFSET_END ? end : cur);
	if (n < 0 || ftruncate(a->rfd, n)) {
		dos_set_ioerr(dos_errno(errno));
		RET(-1);
		return;
	}
	RET(n);
}

static void d_IsInteractive(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	RET(a && a->interactive ? DOSTRUE : DOSFALSE);
}

static void d_WaitForChar(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	fd_set s;
	struct timeval tv;

	if (!a) {
		RET(DOSFALSE);
		return;
	}
	if (a->unget >= 0 || a->rpos < a->rlen || (a->rfd == 0 && con_nreply)) {
		RET(DOSTRUE);
		return;
	}
	dos_flush_all();
	FD_ZERO(&s);
	FD_SET(a->rfd, &s);
	tv.tv_sec = D(2) / 1000000;
	tv.tv_usec = D(2) % 1000000;
	RET(select(a->rfd + 1, &s, NULL, NULL, &tv) > 0 ? DOSTRUE : DOSFALSE);
}

/* ---- buffered I/O ---- */

static void d_FGetC(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	RET(a ? fh_getc(a) : -1);
}

static void d_UnGetC(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	int32_t c = D(2);

	if (!a) {
		RET(0);
		return;
	}
	if (c == -1)
		c = a->last;
	a->unget = c & 0xff;
	RET(DOSTRUE);
}

static void d_FPutC(struct frame *f)
{
	struct afh *a = fh_priv(D(1));

	if (!a || fh_putc(a, D(2) & 0xff) < 0) {
		RET(-1);
		return;
	}
	RET(D(2) & 0xff);
}

static void d_FPuts(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	const char *s = PTR(D(2));

	if (!a) {
		RET(-1);
		return;
	}
	while (*s)
		if (fh_putc(a, (unsigned char)*s++) < 0) {
			RET(-1);
			return;
		}
	RET(0);
}

static void d_PutStr(struct frame *f)
{
	D(2) = D(1);
	D(1) = me->pr_COS;
	d_FPuts(f);
}

static void d_WriteChars(struct frame *f)
{
	struct afh *a = fh_priv(me->pr_COS);
	const char *s = PTR(D(1));
	uint32_t i;

	for (i = 0; a && i < D(2); i++)
		fh_putc(a, (unsigned char)s[i]);
	RET(a ? (int32_t)D(2) : -1);
}

static void d_FGets(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	char *buf = PTR(D(2));
	int32_t len = D(3), n = 0;
	int c = 0;

	if (!a || len <= 0) {
		RET(0);
		return;
	}
	while (n < len - 1 && (c = fh_getc(a)) >= 0) {
		buf[n++] = c;
		if (c == '\n')
			break;
	}
	buf[n] = 0;
	RET(n ? D(2) : 0);
}

static void d_FRead(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	uint8_t *buf = PTR(D(2));
	uint32_t bs = D(3), nb = D(4), i, j;
	int c;

	if (!a || !bs) {
		RET(0);
		return;
	}
	for (i = 0; i < nb; i++)
		for (j = 0; j < bs; j++) {
			if ((c = fh_getc(a)) < 0) {
				RET(i);
				return;
			}
			*buf++ = c;
		}
	RET(nb);
}

static void d_FWrite(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	const uint8_t *buf = PTR(D(2));
	uint32_t bs = D(3), nb = D(4), i, j;

	if (!a) {
		RET(0);
		return;
	}
	for (i = 0; i < nb; i++)
		for (j = 0; j < bs; j++)
			if (fh_putc(a, *buf++) < 0) {
				RET(i);
				return;
			}
	RET(nb);
}

static void d_Flush(struct frame *f)
{
	struct afh *a = fh_priv(D(1));

	if (a) {
		fh_flush(a);
		if (a->rlen)
			fh_unread(a);
	}
	RET(DOSTRUE);
}

static void d_SetVBuf(struct frame *f) { RET(0); }

struct cnt_ctx {
	struct afh *a;
	int32_t n;
};

static void put_fh(int c, void *ctx)
{
	struct cnt_ctx *x = ctx;
	if (x->a)
		fh_putc(x->a, c);
	x->n++;
}

static void d_VFPrintf(struct frame *f)
{
	struct cnt_ctx x = { fh_priv(D(1)), 0 };

	if (!x.a) {
		RET(-1);
		return;
	}
	fmt_do(PTR(D(2)), D(3), put_fh, &x, 0);
	RET(x.n);
}

static void d_VPrintf(struct frame *f)
{
	D(3) = D(2);
	D(2) = D(1);
	D(1) = me->pr_COS;
	d_VFPrintf(f);
}

/* BCPL-style: %S string, %N number, %C char, %X/%O/%I/%T with width */
static void d_VFWritef(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	const char *p = PTR(D(2));
	uint32_t *argv = PTR(D(3));
	char buf[64];

	if (!a)
		return;
	for (; *p; p++) {
		int w = 0;
		char t;
		if (*p != '%') {
			fh_putc(a, (unsigned char)*p);
			continue;
		}
		t = *++p;
		if (!t)
			break;
		if (t >= 'a' && t <= 'z')
			t -= 32;
		if (t == 'T' || t == 'I' || t == 'X' || t == 'O') {
			char c = p[1];
			if (c >= '0' && c <= '9')
				w = c - '0', p++;
			else if (c >= 'A' && c <= 'Z')
				w = c - 'A' + 10, p++;
		}
		switch (t) {
		case 'S': case 'T': {
			const char *s = *argv ? PTR(*argv) : "";
			int l = strlen(s);
			argv++;
			while (*s)
				fh_putc(a, (unsigned char)*s++);
			for (; l < w; l++)
				fh_putc(a, ' ');
			break;
		}
		case 'N': case 'I': case 'U':
			snprintf(buf, sizeof(buf), t == 'U' ? "%*u" : "%*d", w,
				 (int)*argv++);
			for (char *s = buf; *s; s++)
				fh_putc(a, *s);
			break;
		case 'X': case 'O':
			snprintf(buf, sizeof(buf), t == 'X' ? "%0*X" : "%0*o", w,
				 (unsigned)*argv++);
			for (char *s = buf; *s; s++)
				fh_putc(a, *s);
			break;
		case 'C':
			fh_putc(a, *argv++ & 0xff);
			break;
		case '$':
			argv++;
			break;
		default:
			fh_putc(a, t);
		}
	}
}

/* ---- errors ---- */

static void d_IoErr(struct frame *f) { RET(me->pr_Result2); }

static void d_SetIoErr(struct frame *f)
{
	RET(me->pr_Result2);
	me->pr_Result2 = D(1);
}

static void d_Fault(struct frame *f)
{
	RET(fault_text(D(1), PTR(D(2)), PTR(D(3)), D(4)));
}

static void d_PrintFault(struct frame *f)
{
	char buf[300];
	struct afh *a = fh_priv(me->pr_CES ? me->pr_CES : me->pr_COS);
	int32_t code = D(1);

	if (code && a) {
		fault_text(code, PTR(D(2)), buf, sizeof(buf));
		for (char *s = buf; *s; s++)
			fh_putc(a, *s);
		fh_putc(a, '\n');
		fh_flush(a);
	}
	me->pr_Result2 = code;
	RET(code ? DOSTRUE : DOSFALSE);
}

/* ---- process ---- */

void dos_exit(int32_t code)
{
	dos_flush_all();
	exit(code < 0 || code > 255 ? 255 : code);
}

static void d_Exit(struct frame *f) { dos_exit(D(1)); }

static void d_Delay(struct frame *f)
{
	extern void exec_sleep(int64_t us);

	exec_sleep((int64_t)(int32_t)D(1) * 20000);
}

static void d_DateStamp(struct frame *f)
{
	date_now(PTR(D(1)));
	RET(D(1));
}

static void d_Cli(struct frame *f) { RET(BADDR(me->pr_CLI)); }
static void d_GetArgStr(struct frame *f) { RET(me->pr_Arguments); }

static void d_SetArgStr(struct frame *f)
{
	RET(me->pr_Arguments);
	me->pr_Arguments = D(1);
}

static int bstr_get(BPTR b, char *buf, int32_t len)
{
	const uint8_t *s = BADDR(b);
	int n;

	if (len <= 0)
		return 0;
	if (!b) {
		buf[0] = 0;
		return 1;
	}
	n = s[0];
	if (n >= len) {
		n = len - 1;
		memcpy(buf, s + 1, n);
		buf[n] = 0;
		return 0;
	}
	memcpy(buf, s + 1, n);
	buf[n] = 0;
	return 1;
}

static int bstr_set(BPTR b, const char *s, int max)
{
	uint8_t *d = BADDR(b);
	size_t n = strlen(s);

	if (!b)
		return 0;
	if ((int)n > max)
		n = max;
	d[0] = n;
	memcpy(d + 1, s, n);
	return 1;
}

/* the CLI structure; a Workbench start has pr_CLI 0 but we keep one */
static struct CommandLineInterface *hidden_cli;

static struct CommandLineInterface *cli(void)
{
	return me->pr_CLI ? BADDR(me->pr_CLI) : hidden_cli;
}

static void d_GetProgramName(struct frame *f)
{
	if (!bstr_get(cli()->cli_CommandName, PTR(D(1)), D(2))) {
		dos_set_ioerr(ERROR_LINE_TOO_LONG);
		RET(DOSFALSE);
		return;
	}
	RET(DOSTRUE);
}

static void d_SetProgramName(struct frame *f)
{
	RET(bstr_set(cli()->cli_CommandName, PTR(D(1)), 255) ? DOSTRUE : 0);
}

static void d_GetPrompt(struct frame *f)
{
	RET(bstr_get(cli()->cli_Prompt, PTR(D(1)), D(2)) ? DOSTRUE : 0);
}

static void d_SetPrompt(struct frame *f)
{
	RET(bstr_set(cli()->cli_Prompt, PTR(D(1)), 255) ? DOSTRUE : 0);
}

static void d_GetCurrentDirName(struct frame *f)
{
	char *n = path_to_amiga(path_cwd());
	int32_t len = D(2);

	if ((int32_t)strlen(n) >= len) {
		if (len > 0)
			((char *)PTR(D(1)))[0] = 0;
		dos_set_ioerr(ERROR_LINE_TOO_LONG);
		RET(DOSFALSE);
	} else {
		strcpy(PTR(D(1)), n);
		RET(DOSTRUE);
	}
	free(n);
}

static void d_SetCurrentDirName(struct frame *f)
{
	RET(bstr_set(cli()->cli_SetName, PTR(D(1)), 255) ? DOSTRUE : 0);
}

static void d_GetProgramDir(struct frame *f) { RET(me->pr_HomeDir); }

static void d_SetProgramDir(struct frame *f)
{
	RET(me->pr_HomeDir);
	me->pr_HomeDir = D(1);
}

static void d_MaxCli(struct frame *f) { RET(1); }
static void d_FindCliProc(struct frame *f) { RET(D(1) == 1 ? APTR_OF(me) : 0); }
static void d_GetConsoleTask(struct frame *f) { RET(con_port); }
static void d_GetFileSysTask(struct frame *f) { RET(fs_port); }
static void d_SetTask(struct frame *f) { RET(fs_port); }

/*
 * System() and Execute(): no shell, but a command line naming an AmigaOS
 * program is run in this process, the way the shell would: the first
 * word is the command (a path, or looked up in the current directory,
 * then C:), then AmigaDOS redirections (<file, >file, >>file), the rest
 * is its argument line.  A command that is not an AmigaOS executable is
 * handed to the Linux shell (/bin/sh -c), so `System("echo hi")` works.
 */
static void d_RunCommand(struct frame *f);
static int bstr_get(BPTR b, char *buf, int32_t len);
static int bstr_set(BPTR b, const char *s, int max);

static const char *FilePart_c(const char *p)
{
	const char *s = strrchr(p, '/');

	if (!s)
		s = strrchr(p, ':');
	return s ? s + 1 : p;
}

static int32_t run_line(struct frame *f, const char *line, BPTR in, BPTR out)
{
	char cmd[256], *args, *lp = NULL, *redir_in = NULL, *redir_out = NULL;
	const char *p = line;
	int append = 0, err, n = 0;
	BPTR seg = 0, oin = me->pr_CIS, oout = me->pr_COS, fin = 0, fout = 0;
	BPTR odir = me->pr_CurrentDir;
	struct CommandLineInterface *c = cli();
	char oldname[256];
	int32_t rc;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '"') {
		for (p++; *p && *p != '"' && n < 255; p++)
			cmd[n++] = *p;
		if (*p == '"')
			p++;
	} else
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && n < 255)
			cmd[n++] = *p++;
	cmd[n] = 0;
	if (!n)
		return 0;
	/* redirections, which come right after the command */
	args = malloc(strlen(p) + 2);
	args[0] = 0;
	for (;;) {
		char tok[256];
		int k = 0, isin, isout;
		while (*p == ' ' || *p == '\t')
			p++;
		isin = *p == '<';
		isout = *p == '>';
		if (!isin && !isout)
			break;
		p++;
		if (isout && *p == '>')
			append = 1, p++;
		while (*p == ' ')
			p++;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && k < 255)
			tok[k++] = *p++;
		tok[k] = 0;
		if (isin)
			redir_in = strdup(tok);
		else
			redir_out = strdup(tok);
	}
	sprintf(args, "%s", p);
	n = strlen(args);
	while (n && (args[n - 1] == '\n' || args[n - 1] == ' '))
		args[--n] = 0;
	strcat(args, "\n");

	if (strchr(cmd, ':') || strchr(cmd, '/'))
		lp = path_to_linux(cmd, NULL);
	else {
		char a[300];
		struct stat st;
		lp = path_to_linux(cmd, NULL);
		if (!lp || stat(lp, &st) || !S_ISREG(st.st_mode)) {
			free(lp);
			snprintf(a, sizeof(a), "C:%s", cmd);
			lp = path_to_linux(a, NULL);
		}
	}
	if (lp)
		seg = hunk_load(lp, &err);
	free(lp);
	if (!seg) {
		/* not an AmigaOS program: the Linux shell */
		char *sh = malloc(strlen(line) + 1);
		dos_flush_all();
		strcpy(sh, line);
		n = strlen(sh);
		while (n && sh[n - 1] == '\n')
			sh[--n] = 0;
		rc = system(sh);
		free(sh);
		free(args);
		free(redir_in);
		free(redir_out);
		if (rc < 0 || !WIFEXITED(rc))
			return -1;
		rc = WEXITSTATUS(rc);
		return rc == 127 ? -1 : rc;
	}
	if (redir_in && !(fin = dos_open(redir_in, MODE_OLDFILE))) {
		rc = -1;
		goto out;
	}
	if (redir_out && !(fout = dos_open(redir_out, append ?
					    MODE_READWRITE : MODE_NEWFILE))) {
		rc = -1;
		goto out;
	}
	if (fout && append) {
		struct afh *a = fh_priv(fout);
		lseek(a->wfd, 0, SEEK_END);
	}
	me->pr_CIS = fin ? fin : in ? in : oin;
	me->pr_COS = fout ? fout : out ? out : oout;
	bstr_get(c->cli_CommandName, oldname, sizeof(oldname));
	bstr_set(c->cli_CommandName, FilePart_c(cmd), 255);
	{
		struct frame g = *f;
		char *amargs = amem_alloc(strlen(args) + 1);
		strcpy(amargs, args);
		g.d[1] = seg;
		g.d[2] = me->pr_StackSize;
		g.d[3] = APTR_OF(amargs);
		g.d[4] = strlen(amargs);
		d_RunCommand(&g);
		rc = g.d[0];
		amem_free(amargs);
	}
	bstr_set(c->cli_CommandName, oldname, 255);
	dos_flush_all();
	if (me->pr_CurrentDir != odir) {        /* the child's cd is its own */
		me->pr_CurrentDir = odir;
		path_set_cwd(lock_path(odir));
	}
out:
	me->pr_CIS = oin;
	me->pr_COS = oout;
	if (fin)
		fh_close(fin);
	if (fout)
		fh_close(fout);
	hunk_unload(seg);
	free(args);
	free(redir_in);
	free(redir_out);
	return rc;
}

static void d_Execute(struct frame *f)
{
	int32_t rc = run_line(f, PTR(D(1)), D(2), D(3));

	RET(rc < 0 ? DOSFALSE : DOSTRUE);
}

#define SYS_Input  0x80000021
#define SYS_Output 0x80000022
#define SYS_Asynch 0x80000023

static void d_SystemTagList(struct frame *f)
{
	struct TagItem *t = PTR(D(2));
	BPTR in = 0, out = 0;

	for (; t && t->ti_Tag; t++) {
		if (t->ti_Tag == 2) {           /* TAG_MORE */
			t = PTR(t->ti_Data);
			if (!t)
				break;
			t--;
			continue;
		}
		if (t->ti_Tag == SYS_Input)
			in = t->ti_Data;
		else if (t->ti_Tag == SYS_Output)
			out = t->ti_Data;
	}
	RET(run_line(f, PTR(D(1)), in, out));
}

static void d_CheckSignal(struct frame *f)
{
	uint32_t got;

	exec_pending_break();
	got = me->pr_Task.tc_SigRecvd & D(1);
	me->pr_Task.tc_SigRecvd &= ~got;
	RET(got);
}

/* ---- seglists ---- */

static void d_LoadSeg(struct frame *f)
{
	char *lp = path_to_linux(PTR(D(1)), NULL);
	int err;
	BPTR s;

	if (!lp) {
		dos_set_ioerr(ERROR_DEVICE_NOT_MOUNTED);
		RET(0);
		return;
	}
	s = hunk_load(lp, &err);
	free(lp);
	dos_set_ioerr(err);
	RET(s);
}

static void d_UnLoadSeg(struct frame *f)
{
	hunk_unload(D(1));
	RET(DOSTRUE);
}

static void d_RunCommand(struct frame *f)
{
	BPTR seg = D(1);
	uint32_t stack = D(2) < 8192 ? 8192 : D(2);
	uint8_t *st = amem_alloc(stack + 16);
	uint32_t *sp = (uint32_t *)(st + stack);
	struct regs r;
	APTR oldargs = me->pr_Arguments;
	uint32_t res;

	if (!seg || !st) {
		RET(-1);
		return;
	}
	memset(&r, 0, sizeof(r));
	*--sp = stack;
	r.a[7] = APTR_OF(sp);
	r.d[0] = D(4);
	r.a[0] = D(3);
	r.a[6] = APTR_OF(SysBase);
	me->pr_Arguments = D(3);
	dos_preload_args(PTR(D(3)), D(4));
	res = call_amiga(&r, APTR_OF((uint32_t *)BADDR(seg) + 1));
	me->pr_Arguments = oldargs;
	amem_free(st);
	RET(res);
}

static void d_FindSegment(struct frame *f) { RET(0); }

/* ---- DOS objects ---- */

static void d_AllocDosObject(struct frame *f)
{
	switch (D(1)) {
	case 0:         /* DOS_FILEHANDLE */
		RET(amem_alloc(sizeof(struct FileHandle)));
		break;
	case 1:         /* DOS_EXALLCONTROL */
		RET(amem_alloc(sizeof(struct ExAllControl) + 16));
		break;
	case 2:         /* DOS_FIB */
		RET(amem_alloc(sizeof(struct FileInfoBlock)));
		break;
	case 3:         /* DOS_STDPKT: a Message followed by the DosPacket */
	{
		uint8_t *m = amem_alloc(20 + 48);
		((uint32_t *)(m + 20))[0] = APTR_OF(m);
		((struct Node *)m)->ln_Name = APTR_OF(m + 20);
		RET(m + 20);
		break;
	}
	case 4: {       /* DOS_CLI */
		struct CommandLineInterface *c = amem_alloc(sizeof(*c));
		c->cli_SetName = MKBADDR(amem_alloc(256));
		c->cli_CommandName = MKBADDR(amem_alloc(256));
		c->cli_CommandFile = MKBADDR(amem_alloc(256));
		c->cli_Prompt = MKBADDR(amem_alloc(256));
		c->cli_DefaultStack = 4096 / 4;
		RET(c);
		break;
	}
	case 5:         /* DOS_RDARGS */
		RET(amem_alloc(sizeof(struct RDArgs)));
		break;
	default:
		RET(0);
	}
}

static void d_FreeDosObject(struct frame *f)
{
	if (!D(2))
		return;
	if (D(1) == 3)
		amem_free((uint8_t *)PTR(D(2)) - 20);
	else
		amem_free(PTR(D(2)));
}

/* ---- names ---- */

static void d_FilePart(struct frame *f)
{
	const char *p = PTR(D(1)), *s;

	if (!p) {
		RET(0);
		return;
	}
	s = strrchr(p, '/');
	if (!s)
		s = strrchr(p, ':');
	RET(s ? s + 1 : p);
}

static void d_PathPart(struct frame *f)
{
	const char *p = PTR(D(1)), *s;

	s = strrchr(p, '/');
	if (s) {
		/* keep leading slashes ("/" alone means the parent) */
		const char *q = s;
		while (q > p && q[-1] == '/')
			q--;
		if (q == p || q[-1] == ':')
			s++;
		RET(s);
		return;
	}
	s = strrchr(p, ':');
	RET(s ? s + 1 : p);
}

static void d_AddPart(struct frame *f)
{
	char *dir = PTR(D(1));
	const char *file = PTR(D(2));
	uint32_t size = D(3);
	char tmp[1024];
	size_t l;

	if (strchr(file, ':')) {
		snprintf(tmp, sizeof(tmp), "%s", file);
	} else {
		snprintf(tmp, sizeof(tmp), "%s", dir);
		l = strlen(tmp);
		while (*file == '/') {          /* parent: strip a component */
			char *s;
			if (l && tmp[l - 1] == '/')
				tmp[--l] = 0;
			s = strrchr(tmp, '/');
			if (!s)
				s = strrchr(tmp, ':');
			if (s && l) {
				l = s - tmp + 1;
				tmp[l] = 0;
				file++;
			} else {
				if (l < sizeof(tmp) - 1)
					tmp[l++] = '/', tmp[l] = 0;
				file++;
			}
		}
		if (l && tmp[l - 1] != ':' && tmp[l - 1] != '/' && *file &&
		    l < sizeof(tmp) - 1)
			tmp[l++] = '/', tmp[l] = 0;
		snprintf(tmp + l, sizeof(tmp) - l, "%s", file);
	}
	if (strlen(tmp) >= size) {
		dos_set_ioerr(ERROR_LINE_TOO_LONG);
		RET(DOSFALSE);
		return;
	}
	strcpy(dir, tmp);
	RET(DOSTRUE);
}

static void d_SplitName(struct frame *f)
{
	const char *name = PTR(D(1));
	char sep = D(2), *buf = PTR(D(3));
	int32_t pos = D(4), size = D(5), n = 0;

	if (pos < 0 || pos > (int32_t)strlen(name)) {
		RET(-1);
		return;
	}
	name += pos;
	while (*name && *name != sep) {
		if (n < size - 1)
			buf[n++] = *name;
		name++, pos++;
	}
	if (size > 0)
		buf[n] = 0;
	if (!*name) {
		RET(-1);
		return;
	}
	RET(pos + 1);
}

static void d_StrToLong(struct frame *f)
{
	const char *s = PTR(D(1)), *p = s;
	int32_t v = 0, neg = 0;
	int any = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '-' || *p == '+')
		neg = *p++ == '-';
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p++ - '0');
		any = 1;
	}
	if (!any) {
		RET(-1);
		return;
	}
	*(int32_t *)PTR(D(2)) = neg ? -v : v;
	RET(p - s);
}

/* ---- variables: local and global both map onto the environment ---- */

static void d_GetVar(struct frame *f)
{
	const char *v = getenv(PTR(D(1)));
	char *buf = PTR(D(2));
	int32_t size = D(3), n;

	if (!v) {
		char *lp, path[512];
		FILE *fp;
		snprintf(path, sizeof(path), "ENV:%s", (char *)PTR(D(1)));
		lp = path_to_linux(path, NULL);
		if (lp && !(D(4) & 0x100) && (fp = fopen(lp, "r"))) {
			n = fread(buf, 1, size > 0 ? size - 1 : 0, fp);
			fclose(fp);
			free(lp);
			while (n && (buf[n - 1] == '\n'))
				n--;
			if (size > 0)
				buf[n] = 0;
			RET(n);
			return;
		}
		free(lp);
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
		RET(-1);
		return;
	}
	n = strlen(v);
	if (size <= 0) {
		RET(0);
		return;
	}
	if (n >= size)
		n = size - 1;
	memcpy(buf, v, n);
	buf[n] = 0;
	RET(n);
}

static void d_SetVar(struct frame *f)
{
	char *val;
	int32_t size = D(3);

	if (!D(2)) {
		unsetenv(PTR(D(1)));
		RET(DOSTRUE);
		return;
	}
	if (size < 0)
		size = strlen(PTR(D(2)));
	val = strndup(PTR(D(2)), size);
	setenv(PTR(D(1)), val, 1);
	free(val);
	RET(DOSTRUE);
}

static void d_DeleteVar(struct frame *f)
{
	unsetenv(PTR(D(1)));
	RET(DOSTRUE);
}

static void d_FindVar(struct frame *f) { RET(0); }

static void d_ErrorReport(struct frame *f) { RET(DOSTRUE); }

/*
 * DoPkt(): no handlers here, so the few actions programs send to ask
 * questions are answered directly and the rest fail politely.
 */
#define ACTION_CURRENT_VOLUME 7
#define ACTION_DISK_INFO      25
#define ACTION_INFO           26
#define ACTION_IS_FILESYSTEM  1027

static void d_DoPkt(struct frame *f)
{
	extern int lock_info(BPTR lock, struct InfoData *id);
	extern int path_info(const char *lpath, struct InfoData *id);
	extern const char *port_path(APTR port);
	const char *vp = port_path(D(1));

	if (D(1) == APTR_OF(con_port)) {
		/* the console: DISK_INFO is how programs find its window
		   (id_VolumeNode); there is none */
		if (D(2) == ACTION_DISK_INFO) {
			struct InfoData *id = BADDR(D(3));
			memset(id, 0, sizeof(*id));
			id->id_DiskType = 0x434f4e00;   /* ID_CON */
			id->id_DiskState = 82;
			D(1) = 0;
			RET(DOSTRUE);
			return;
		}
		if (D(2) == ACTION_IS_FILESYSTEM) {
			D(1) = 0;
			RET(DOSFALSE);
			return;
		}
	}
	switch (D(2)) {
	case ACTION_IS_FILESYSTEM:
		D(1) = 0;
		RET(DOSTRUE);
		return;
	case ACTION_DISK_INFO:
		if (vp)
			path_info(vp, BADDR(D(3)));
		else
			lock_info(0, BADDR(D(3)));
		D(1) = 0;
		RET(DOSTRUE);
		return;
	case ACTION_INFO:
		lock_info(D(3), BADDR(D(4)));
		D(1) = 0;
		RET(DOSTRUE);
		return;
	}
	if (af_trace)
		af_warn("DoPkt action %d refused", (int)D(2));
	dos_set_ioerr(ERROR_ACTION_NOT_KNOWN);
	D(1) = ERROR_ACTION_NOT_KNOWN;
	RET(DOSFALSE);
}

/* SetMode(fh, 1): a raw console, as termios non-canonical, no echo */
static struct termios saved_tio;
static int tio_saved = -1;

static void restore_tio(void)
{
	if (tio_saved >= 0)
		tcsetattr(tio_saved, TCSANOW, &saved_tio);
}

static void d_SetMode(struct frame *f)
{
	struct afh *a = fh_priv(D(1));
	struct termios t;
	int fd = a ? a->rfd : -1;

	if (fd < 0 || !isatty(fd) || tcgetattr(fd, &t)) {
		RET(DOSFALSE);
		return;
	}
	if (tio_saved < 0) {
		saved_tio = t;
		tio_saved = fd;
		atexit(restore_tio);
	}
	if (D(2)) {
		t.c_lflag &= ~(ICANON | ECHO);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
	} else
		t = saved_tio;
	tcsetattr(fd, TCSANOW, &t);
	RET(DOSTRUE);
}

static void d_DosGetString(struct frame *f)
{
	static char buf[128];

	fault_text(D(1), NULL, buf, sizeof(buf));
	RET(buf);
}

static void d_Zero(struct frame *f) { RET(0); }
static void d_True(struct frame *f) { RET(DOSTRUE); }

static void d_NotImpl(struct frame *f)
{
	dos_set_ioerr(ERROR_NOT_IMPLEMENTED);
	RET(DOSFALSE);
}

/* the shell's ReadArgs and friends */
static void d_ReadArgs(struct frame *f)
{
	RET(ra_readargs(PTR(D(1)), PTR(D(2)), PTR(D(3))));
}

static void d_FreeArgs(struct frame *f) { ra_freeargs(PTR(D(1))); }

static void d_ReadItem(struct frame *f)
{
	RET(ra_readitem(PTR(D(1)), D(2), PTR(D(3))));
}

static void d_FindArg(struct frame *f)
{
	RET(ra_findarg(PTR(D(1)), PTR(D(2))));
}

static void d_ParsePattern(struct frame *f)
{
	RET(pat_parse(PTR(D(1)), PTR(D(2)), D(3), 0));
}

static void d_ParsePatternNoCase(struct frame *f)
{
	RET(pat_parse(PTR(D(1)), PTR(D(2)), D(3), 1));
}

static void d_MatchPattern(struct frame *f)
{
	RET(pat_match(PTR(D(1)), PTR(D(2)), 0) ? DOSTRUE : DOSFALSE);
}

static void d_MatchPatternNoCase(struct frame *f)
{
	RET(pat_match(PTR(D(1)), PTR(D(2)), 1) ? DOSTRUE : DOSFALSE);
}

/* ---- CreateNewProc / CreateProc: a process on a task of its own (task.c) ---- */

struct atask;
extern struct atask *task_add(struct Task *t, uint32_t initpc, uint32_t finalpc,
			      void (*cstart)(struct atask *));
extern uint32_t task_initpc(struct atask *a);
extern struct Task *task_of(struct atask *a);
extern struct TagItem *util_next_tag(struct TagItem **tp);

/* the process's entry, its arguments, then pr_ExitCode(result, ExitData) */
static void process_start(struct atask *at)
{
	struct Process *p = (struct Process *)task_of(at);
	struct regs r;
	uint32_t rc;
	const char *args = p->pr_Arguments ? PTR(p->pr_Arguments) : "";

	memset(&r, 0, sizeof(r));
	r.d[0] = strlen(args);
	r.a[0] = APTR_OF(args);
	r.a[6] = APTR_OF(SysBase);
	r.a[7] = p->pr_Task.tc_SPReg;
	rc = call_amiga(&r, task_initpc(at));
	if (p->pr_ExitCode) {
		memset(&r, 0, sizeof(r));
		r.d[0] = rc;
		r.d[1] = p->pr_ExitData;
		r.a[6] = APTR_OF(SysBase);
		r.a[7] = p->pr_Task.tc_SPReg;
		call_amiga(&r, p->pr_ExitCode);
	}
}

static struct Process *new_process(const char *name, int pri, uint32_t entry, uint32_t stack,
				   struct TagItem *tags)
{
	struct Process *p = amem_alloc(sizeof(*p) + 64);
	char *nm = amem_alloc(strlen(name) + 1);
	uint8_t *st;
	uint32_t *sp;
	struct TagItem *t = tags, *x;
	BPTR in = me->pr_CIS, out = me->pr_COS, err = me->pr_CES;

	strcpy(nm, name);
	p->pr_Task.tc_Node.ln_Type = NT_PROCESS;
	p->pr_Task.tc_Node.ln_Name = APTR_OF(nm);
	p->pr_Task.tc_Node.ln_Pri = pri;
	p->pr_Task.tc_SigAlloc = 0xffff;
	exec_newlist(&p->pr_Task.tc_MemEntry);
	p->pr_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
	p->pr_MsgPort.mp_SigBit = 8;
	p->pr_MsgPort.mp_SigTask = APTR_OF(p);
	exec_newlist(&p->pr_MsgPort.mp_MsgList);
	exec_newlist((struct List *)&p->pr_LocalVars);
	p->pr_CurrentDir = me->pr_CurrentDir;
	p->pr_HomeDir = me->pr_HomeDir;
	p->pr_ConsoleTask = me->pr_ConsoleTask;
	p->pr_FileSystemTask = me->pr_FileSystemTask;
	p->pr_WindowPtr = me->pr_WindowPtr;
	p->pr_GlobVec = me->pr_GlobVec;
	p->pr_TaskNum = 0;
	while ((x = util_next_tag(&t))) {
		switch (x->ti_Tag - 0x800003e8) {
		case 4: in = x->ti_Data; break;
		case 5: out = x->ti_Data; break;
		case 8: err = x->ti_Data; break;
		case 10: p->pr_CurrentDir = x->ti_Data; break;
		case 14: p->pr_ConsoleTask = x->ti_Data; break;
		case 15: p->pr_WindowPtr = x->ti_Data; break;
		case 16: p->pr_HomeDir = x->ti_Data; break;
		case 21: p->pr_Arguments = x->ti_Data; break;
		case 24: p->pr_ExitCode = x->ti_Data; break;
		case 25: p->pr_ExitData = x->ti_Data; break;
		}
	}
	p->pr_CIS = in;
	p->pr_COS = out;
	p->pr_CES = err;
	if (stack < 4096)
		stack = 4096;
	st = amem_alloc(stack + 64);
	p->pr_StackSize = stack;
	p->pr_Task.tc_SPLower = APTR_OF(st);
	p->pr_Task.tc_SPUpper = APTR_OF(st + stack);
	p->pr_StackBase = MKBADDR(st + stack);
	sp = (uint32_t *)(st + stack);
	*--sp = stack;                          /* 4(sp) = the stack size */
	p->pr_Task.tc_SPReg = APTR_OF(sp);
	if (!task_add(&p->pr_Task, entry, 0, process_start))
		return NULL;
	return p;
}

static void d_CreateNewProc(struct frame *f)
{
	struct TagItem *tags = PTR(D(1)), *t = tags, *x;
	uint32_t entry = 0, stack = 4000, name = 0;
	int pri = 0;
	struct Process *p;

	while ((x = util_next_tag(&t))) {
		switch (x->ti_Tag - 0x800003e8) {
		case 1: entry = APTR_OF((uint32_t *)BADDR(x->ti_Data) + 1); break;
		case 3: entry = x->ti_Data; break;
		case 11: stack = x->ti_Data; break;
		case 12: name = x->ti_Data; break;
		case 13: pri = (int8_t)x->ti_Data; break;
		}
	}
	if (!entry) {
		RET(0);
		return;
	}
	p = new_process(name ? PTR(name) : "New Process", pri, entry, stack, tags);
	RET(p);
}

/* CreateProc(name, pri, seglist, stack): the old one, answers the MsgPort */
static void d_CreateProc(struct frame *f)
{
	struct Process *p = new_process(D(1) ? PTR(D(1)) : "New Process", (int8_t)D(2),
					APTR_OF((uint32_t *)BADDR(D(3)) + 1), D(4), NULL);

	RET(p ? APTR_OF(&p->pr_MsgPort) : 0);
}

const struct libimpl dos_impl_io[] = {
	{ "CreateNewProc", d_CreateNewProc }, { "CreateProc", d_CreateProc },
	{ "Open", d_Open }, { "Close", d_Close },
	{ "Read", d_Read }, { "Write", d_Write },
	{ "Input", d_Input }, { "Output", d_Output },
	{ "SelectInput", d_SelectInput }, { "SelectOutput", d_SelectOutput },
	{ "Seek", d_Seek }, { "SetFileSize", d_SetFileSize },
	{ "IsInteractive", d_IsInteractive }, { "WaitForChar", d_WaitForChar },
	{ "FGetC", d_FGetC }, { "UnGetC", d_UnGetC }, { "FPutC", d_FPutC },
	{ "FPuts", d_FPuts }, { "PutStr", d_PutStr },
	{ "WriteChars", d_WriteChars }, { "FGets", d_FGets },
	{ "FRead", d_FRead }, { "FWrite", d_FWrite }, { "Flush", d_Flush },
	{ "SetVBuf", d_SetVBuf },
	{ "VFPrintf", d_VFPrintf }, { "VPrintf", d_VPrintf },
	{ "VFWritef", d_VFWritef },
	{ "IoErr", d_IoErr }, { "SetIoErr", d_SetIoErr },
	{ "Fault", d_Fault }, { "PrintFault", d_PrintFault },
	{ "ErrorReport", d_ErrorReport },
	{ "DoPkt", d_DoPkt }, { "SetMode", d_SetMode },
	{ "DosGetString", d_DosGetString },
	{ "LockRecord", d_True }, { "LockRecords", d_True },
	{ "UnLockRecord", d_True }, { "UnLockRecords", d_True },
	{ "AddSegment", d_Zero }, { "RemSegment", d_Zero },
	{ "Exit", d_Exit }, { "Delay", d_Delay }, { "DateStamp", d_DateStamp },
	{ "Cli", d_Cli }, { "GetArgStr", d_GetArgStr },
	{ "SetArgStr", d_SetArgStr },
	{ "GetProgramName", d_GetProgramName },
	{ "SetProgramName", d_SetProgramName },
	{ "GetPrompt", d_GetPrompt }, { "SetPrompt", d_SetPrompt },
	{ "GetCurrentDirName", d_GetCurrentDirName },
	{ "SetCurrentDirName", d_SetCurrentDirName },
	{ "GetProgramDir", d_GetProgramDir },
	{ "SetProgramDir", d_SetProgramDir },
	{ "MaxCli", d_MaxCli }, { "FindCliProc", d_FindCliProc },
	{ "GetConsoleTask", d_GetConsoleTask },
	{ "GetFileSysTask", d_GetFileSysTask },
	{ "SetConsoleTask", d_SetTask }, { "SetFileSysTask", d_SetTask },
	{ "Execute", d_Execute }, { "SystemTagList", d_SystemTagList },
	{ "CheckSignal", d_CheckSignal },
	{ "LoadSeg", d_LoadSeg }, { "NewLoadSeg", d_LoadSeg },
	{ "UnLoadSeg", d_UnLoadSeg }, { "RunCommand", d_RunCommand },
	{ "FindSegment", d_FindSegment },
	{ "AllocDosObject", d_AllocDosObject },
	{ "FreeDosObject", d_FreeDosObject },
	{ "FilePart", d_FilePart }, { "PathPart", d_PathPart },
	{ "AddPart", d_AddPart }, { "SplitName", d_SplitName },
	{ "StrToLong", d_StrToLong },
	{ "GetVar", d_GetVar }, { "SetVar", d_SetVar },
	{ "DeleteVar", d_DeleteVar }, { "FindVar", d_FindVar },
	{ "ReadArgs", d_ReadArgs }, { "FreeArgs", d_FreeArgs },
	{ "ReadItem", d_ReadItem }, { "FindArg", d_FindArg },
	{ "ParsePattern", d_ParsePattern },
	{ "ParsePatternNoCase", d_ParsePatternNoCase },
	{ "MatchPattern", d_MatchPattern },
	{ "MatchPatternNoCase", d_MatchPatternNoCase },
	{ "IsFileSystem", d_True },
	{ "StartNotify", d_NotImpl }, { "EndNotify", d_Zero },
	{ "AddBuffers", d_NotImpl }, { "Inhibit", d_NotImpl },
	{ "Relabel", d_NotImpl }, { "Format", d_NotImpl },
	{ NULL, NULL }
};

/* ---- the process ---- */

static BPTR bstr_new(const char *s, int room)
{
	uint8_t *b = amem_alloc(room + 1);
	size_t n = strlen(s);

	if (n > (size_t)room)
		n = room;
	b[0] = n;
	memcpy(b + 1, s, n);
	return MKBADDR(b);
}

static void dos_setup(struct Library *b)
{
	struct DosLibrary *d = (struct DosLibrary *)b;
	struct RootNode *rn = amem_alloc(sizeof(*rn));

	DOSBase = d;
	d->dl_Root = APTR_OF(rn);
	date_now(&rn->rn_Time);
	exec_newlist(&rn->rn_CliList);
	d->dl_UtilityBase = APTR_OF(lib_build(&lib_utility));
	fs_port = amem_alloc(sizeof(*fs_port));
	fs_port->mp_Node.ln_Type = NT_MSGPORT;
	fs_port->mp_Flags = 2;                          /* PA_IGNORE */
	exec_newlist(&fs_port->mp_MsgList);
	con_port = amem_alloc(sizeof(*con_port));
	*con_port = *fs_port;
	exec_newlist(&con_port->mp_MsgList);
}

extern const struct libimpl dos_impl_lock[], dos_impl_date[];
static struct libimpl dos_impl[256];

struct amilib lib_dos = {
	.name = "dos.library", .version = 40, .revision = 3,
	.fd = fd_dos, .impl = dos_impl,
	.possize = sizeof(struct DosLibrary) - sizeof(struct Library),
	.init = dos_setup,
};

__attribute__((constructor)) static void dos_merge_impl(void)
{
	int n = 0;

	for (const struct libimpl *i = dos_impl_io; i->name; i++)
		dos_impl[n++] = *i;
	for (const struct libimpl *i = dos_impl_lock; i->name; i++)
		dos_impl[n++] = *i;
	for (const struct libimpl *i = dos_impl_date; i->name; i++)
		dos_impl[n++] = *i;
}

/*
 * Rebuild the argument line from Linux's argv the way the AmigaOS shell
 * would hand it over: words separated by spaces, quoted when they need
 * it (with * escapes), and a newline at the end.
 */
static char *make_args(int argc, char **argv)
{
	size_t cap = 2, n = 0;
	char *s;
	int i;

	for (i = 0; i < argc; i++)
		cap += strlen(argv[i]) * 2 + 3;
	s = amem_alloc(cap + 1);
	for (i = 0; i < argc; i++) {
		const char *a = argv[i];
		int q = !*a || strpbrk(a, " \t\n\"") != NULL;
		if (i)
			s[n++] = ' ';
		if (q)
			s[n++] = '"';
		for (; *a; a++) {
			if (q && (*a == '"' || *a == '*'))
				s[n++] = '*', s[n++] = *a;
			else if (q && *a == '\n')
				s[n++] = '*', s[n++] = 'N';
			else
				s[n++] = *a;
		}
		if (q)
			s[n++] = '"';
	}
	s[n++] = '\n';
	s[n] = 0;
	dos_arglen = n;
	return s;
}

/*
 * Started from a Workbench icon (wbde puts the .info's path in WBDE_ICON):
 * no CLI, no standard streams, and a WBStartup message waiting on the
 * process port with the tool and any projects as WBArgs.  argv holds the
 * projects (wbde runs the default tool with the project as an argument).
 */
static void wb_start(struct Process *p, int argc, char **argv,
		     const char *prog, BPTR seg)
{
	struct WBStartup *wb = amem_alloc(sizeof(*wb));
	struct WBArg *args = amem_alloc((argc + 1) * sizeof(*args));
	struct MsgPort *rp = amem_alloc(sizeof(*rp));
	char full[4096], *s;
	int i;

	unsetenv("WBDE_ICON");          /* not for programs this one starts */
	rp->mp_Node.ln_Type = NT_MSGPORT;
	rp->mp_Flags = 2;                       /* PA_IGNORE */
	exec_newlist(&rp->mp_MsgList);
	for (i = 0; i <= argc; i++) {
		const char *path = i ? argv[i - 1] : prog;
		char *name;
		if (!realpath(path, full))
			snprintf(full, sizeof(full), "%s", path);
		s = strrchr(full, '/');
		name = amem_alloc(strlen(s ? s + 1 : full) + 1);
		strcpy(name, s ? s + 1 : full);
		if (s == full)
			s[1] = 0;
		else if (s)
			*s = 0;
		args[i].wa_Lock = lock_new(s ? full : ".", SHARED_LOCK);
		args[i].wa_Name = APTR_OF(name);
	}
	wb->sm_Message.mn_ReplyPort = APTR_OF(rp);
	wb->sm_Message.mn_Length = sizeof(*wb);
	wb->sm_Process = APTR_OF(&p->pr_MsgPort);
	wb->sm_Segment = seg;
	wb->sm_NumArgs = argc + 1;
	wb->sm_ArgList = APTR_OF(args);
	p->pr_CLI = 0;
	p->pr_CIS = p->pr_COS = 0;
	dos_args = amem_alloc(2);
	dos_arglen = 0;
	p->pr_Arguments = APTR_OF(dos_args);
	exec_putmsg(&p->pr_MsgPort, &wb->sm_Message);
}

void dos_init(int argc, char **argv, const char *prog, BPTR seg,
	      uint32_t stack)
{
	struct Process *p = amem_alloc(sizeof(*p) + 64);
	struct CommandLineInterface *c = amem_alloc(sizeof(*c));
	uint32_t *segarray = amem_alloc(16);
	const char *base = strrchr(prog, '/');
	char *raw = getenv("AMIGAFMT_RAWARGS");
	char *name = amem_alloc(strlen(prog) + 1);

	strcpy(name, base ? base + 1 : prog);
	lib_build(&lib_dos);
	me = p;
	SysBase->ThisTask = APTR_OF(p);
	p->pr_Task.tc_Node.ln_Type = NT_PROCESS;
	p->pr_Task.tc_Node.ln_Name = APTR_OF(name);
	p->pr_Task.tc_State = 2;                        /* TS_RUN */
	p->pr_Task.tc_SigAlloc = 0xffff;
	exec_newlist(&p->pr_Task.tc_MemEntry);
	p->pr_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
	p->pr_MsgPort.mp_SigBit = 8;
	p->pr_MsgPort.mp_SigTask = APTR_OF(p);
	exec_newlist(&p->pr_MsgPort.mp_MsgList);
	exec_newlist((struct List *)&p->pr_LocalVars);
	p->pr_StackSize = stack;
	segarray[0] = 3;
	segarray[3] = seg;
	p->pr_SegList = MKBADDR(segarray);
	p->pr_TaskNum = 1;
	p->pr_GlobVec = APTR_OF(amem_alloc(1024));    /* BCPL global vector */
	p->pr_CIS = fh_new(0, -1, 1, NULL);
	p->pr_COS = fh_new(-1, 1, 1, NULL);
	p->pr_CES = fh_new(-1, 2, 1, NULL);
	p->pr_ConsoleTask = APTR_OF(con_port);
	p->pr_FileSystemTask = APTR_OF(fs_port);
	p->pr_WindowPtr = 0;
	p->pr_CLI = MKBADDR(c);

	c->cli_CommandName = bstr_new(base ? base + 1 : prog, 255);
	c->cli_SetName = bstr_new("", 255);
	c->cli_Prompt = bstr_new("%N> ", 255);
	c->cli_CommandFile = bstr_new("", 255);
	c->cli_StandardInput = c->cli_CurrentInput = p->pr_CIS;
	c->cli_StandardOutput = c->cli_CurrentOutput = p->pr_COS;
	c->cli_Interactive = isatty(0) ? DOSTRUE : 0;
	c->cli_DefaultStack = stack / 4;
	c->cli_FailLevel = 10;
	c->cli_Module = seg;

	lock_init_process(p, prog);

	hidden_cli = c;
	if (getenv("WBDE_ICON") && *getenv("WBDE_ICON")) {
		wb_start(p, argc, argv, prog, seg);
		return;
	}
	if (raw) {
		dos_args = amem_alloc(strlen(raw) + 2);
		sprintf(dos_args, "%s\n", raw);
		dos_arglen = strlen(dos_args);
	} else
		dos_args = make_args(argc, argv);
	p->pr_Arguments = APTR_OF(dos_args);
	dos_preload_args(dos_args, dos_arglen);
}

/*
 * The shell also leaves the argument line in Input()'s buffer (RunCommand
 * does), which is where ReadArgs() takes it from and where programs using
 * ReadItem(..., NULL) find it: C:Execute does that.  A program that reads
 * Input() without parsing its arguments sees the line first, as on AmigaOS.
 */
static int32_t preload_end = -1;        /* where the line ends in rbuf */

void dos_preload_args(const char *s, uint32_t len)
{
	struct afh *a = fh_priv(me->pr_CIS);

	if (!a || len > sizeof(a->rbuf))
		return;
	memcpy(a->rbuf, s, len);
	a->rpos = 0;
	a->rlen = len;
	a->unget = -1;
	preload_end = len;
}

/* for ReadArgs: what is left of the preloaded line, consumed */
int dos_take_args(const char **s, int32_t *len)
{
	struct afh *a = fh_priv(me->pr_CIS);

	if (!a || preload_end < 0 || a->rpos >= preload_end ||
	    a->rlen < preload_end)
		return 0;
	*s = (const char *)a->rbuf + a->rpos;
	*len = preload_end - a->rpos;
	a->rpos = preload_end;
	preload_end = -1;
	return 1;
}
