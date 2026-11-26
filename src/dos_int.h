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
void dos_close(BPTR fh);
struct CommandLineInterface *dos_cli(void);
int dos_fault_text(int32_t code, const char *hdr, char *buf, int32_t len);
int32_t dos_runcommand(BPTR seg, uint32_t stack, const char *args,
		       uint32_t len);
void dos_set_reslist(BPTR head);
APTR dos_fs_port(void);
void lock_free(BPTR lock);

/* the shell (shell.c) */
int32_t shell_system(const char *line, BPTR in, BPTR out, int commands);

void lock_init_process(struct Process *p, const char *prog);
const char *lock_path(BPTR lock);
BPTR lock_new(const char *lpath, int32_t mode);

/* variables (var.c) */
struct LocalVar;
struct LocalVar *var_find(const char *name, int type);
int var_set_local(const char *name, const char *val, int32_t len, int type);
int var_delete_local(const char *name, int type);
char *var_get_global(const char *name, int32_t *len);
int var_set_global(const char *name, const char *val, int32_t len, int save);
int var_delete_global(const char *name);

#endif
