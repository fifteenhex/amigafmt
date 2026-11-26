/* dos.library internals shared by dos.c, lock.c and readargs.c */
#ifndef DOS_INT_H
#define DOS_INT_H

#include "af.h"

struct afh {
	int rfd, wfd;
	int keep;               /* one of the shell's streams: never closed */
	int interactive;
	int seekable;           /* one regular file: read-ahead can be undone */
	int tty_out;            /* writes go to a terminal: translate CSI */
	char *path;             /* Linux path, for NameFromFH and friends */
	struct FileHandle *fh;
	int rpos, rlen, wlen;
	int unget, last;
	unsigned char rbuf[4096], wbuf[4096];
};

struct afh *fh_priv(BPTR b);
BPTR fh_new(int rfd, int wfd, int keep, const char *path);
int fh_flush(struct afh *a);
int fh_getc(struct afh *a);
int fh_putc(struct afh *a, int c);
int fh_write(struct afh *a, const void *buf, int32_t len);
int32_t fh_read(struct afh *a, void *buf, int32_t len);
BPTR dos_open(const char *name, int32_t mode);

void lock_init_process(struct Process *p, const char *prog);
const char *lock_path(BPTR lock);
BPTR lock_new(const char *lpath, int32_t mode);

void exec_newlist(void *l);
void exec_addtail(void *l, void *n);

#endif
