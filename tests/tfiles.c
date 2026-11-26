/* files, locks, directories, MatchFirst; works inside T:amigafmt-test */
#include "t.h"

#define MODE_NEWFILE 1006
#define MODE_OLDFILE 1005
#define SHARED_LOCK (-2)

struct FIB {
	u32 key; s32 type; char name[108]; s32 prot, etype; u32 size;
	s32 blocks; s32 days, mins, ticks; char comment[80]; u16 uid, gid;
	char reserved[32];
};

static struct FIB fib __attribute__((aligned(4)));
static struct { u32 base, last; s32 bb, fb; u8 flags, res; s16 strlen;
	struct FIB info; char buf[256]; } ap __attribute__((aligned(4)));

static u32 dos(int off, const char *r, u32 a, u32 b, u32 c)
{
	return call(DOSBase, off, r, a, b, c);
}

static void err(const char *what)
{
	char buf[100];
	call(DOSBase, LVOFault, "d1d2d3d4", call(DOSBase, LVOIoErr, ""), what,
	     buf, sizeof(buf));
	printf("%s\n", buf);
}

int main(u32 arglen, char *args)
{
	u32 fh, lock, old, n;
	char buf[64];
	int count = 0;

	dos(LVODeleteFile, "d1", (u32)"T:amigafmt-test/a.txt", 0, 0);
	dos(LVODeleteFile, "d1", (u32)"T:amigafmt-test/b.txt", 0, 0);
	dos(LVODeleteFile, "d1", (u32)"T:amigafmt-test/sub", 0, 0);
	dos(LVODeleteFile, "d1", (u32)"T:amigafmt-test", 0, 0);
	lock = dos(LVOCreateDir, "d1", (u32)"T:amigafmt-test", 0, 0);
	if (!lock) {
		err("CreateDir");
		return 20;
	}
	old = dos(LVOCurrentDir, "d1", lock, 0, 0);

	fh = dos(LVOOpen, "d1d2", (u32)"a.txt", MODE_NEWFILE, 0);
	printf("open %s\n", fh ? "ok" : "failed");
	n = dos(LVOWrite, "d1d2d3", fh, (u32)"hello file\nline 2\n", 18);
	printf("wrote %ld\n", n);
	printf("seek was %ld\n", dos(LVOSeek, "d1d2d3", fh, 6, (u32)-1));
	n = dos(LVORead, "d1d2d3", fh, (u32)buf, 4);
	buf[n] = 0;
	printf("read %ld [%s]\n", n, buf);
	dos(LVOClose, "d1", fh, 0, 0);

	fh = dos(LVOOpen, "d1d2", (u32)"A.TXT", MODE_OLDFILE, 0);
	printf("case-insensitive open %s\n", fh ? "ok" : "failed");
	while (dos(LVOFGets, "d1d2d3", fh, (u32)buf, sizeof(buf)))
		printf("FGets: %s", buf);
	dos(LVOClose, "d1", fh, 0, 0);

	printf("rename %ld\n", dos(LVORename, "d1d2", (u32)"a.txt",
				    (u32)"b.txt", 0));
	printf("open missing: %s\n",
	       dos(LVOOpen, "d1d2", (u32)"a.txt", MODE_OLDFILE, 0) ? "?" : "no");
	err("a.txt");
	dos(LVOUnLock, "d1", dos(LVOCreateDir, "d1", (u32)"sub", 0, 0), 0, 0);

	dos(LVOExamine, "d1d2", lock, (u32)&fib, 0);
	printf("examine: %s type %ld\n", fib.name, fib.type);
	while (dos(LVOExNext, "d1d2", lock, (u32)&fib, 0))
		count += fib.type < 0 ? 1 : 100;
	printf("exnext: %ld (files + 100*dirs)\n", count);
	err("ExNext end");

	{
		u32 l2 = dos(LVOLock, "d1d2", (u32)"/amigafmt-test/sub",
			     SHARED_LOCK, 0);
		dos(LVONameFromLock, "d1d2d3", l2, (u32)buf, sizeof(buf));
		printf("NameFromLock ends with: %s\n",
		       buf + strlen(buf) - 18);
		dos(LVOUnLock, "d1", l2, 0, 0);
	}

	ap.strlen = sizeof(ap.buf);
	n = dos(LVOMatchFirst, "d1d2", (u32)"#?.TXT", (u32)&ap, 0);
	while (!n) {
		printf("match: %s (%s)\n", ap.info.name, ap.buf);
		n = dos(LVOMatchNext, "d1", (u32)&ap, 0, 0);
	}
	dos(LVOMatchEnd, "d1", (u32)&ap, 0, 0);
	printf("match end %ld\n", n);

	printf("delete dir with file: %ld\n",
	       dos(LVODeleteFile, "d1", (u32)"/amigafmt-test", 0, 0));
	err("Delete");
	dos(LVODeleteFile, "d1", (u32)"b.txt", 0, 0);
	dos(LVODeleteFile, "d1", (u32)"sub", 0, 0);
	dos(LVOCurrentDir, "d1", old, 0, 0);
	dos(LVOUnLock, "d1", lock, 0, 0);
	printf("delete dir: %ld\n",
	       dos(LVODeleteFile, "d1", (u32)"T:amigafmt-test", 0, 0));
	return 0;
}
