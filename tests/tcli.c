/* helpers for the shell tests:
     tcli cat            copy Input() to Output()
     tcli script <file>  make <file> the CLI's current input, as C:Execute
                         does, so the shell runs it when this returns
     tcli exit <n>       Exit(n) from inside a nested function
     tcli internal       run the shell's Echo through FindSegment/RunCommand
     tcli vars           GetVar/SetVar, local and global
     tcli names          utility.library's named objects
     tcli keymap         MapRawKey through a keymap's tables
     tcli pkt            SendPkt and WaitPkt to a filesystem and the console
     tcli cd             CD through System(), in a directory with "boards"
                         and "devices" (the machine-sysfs board directory)
     tcli proc           CreateNewProc() copies the current directory lock,
                         in the same directory */
#include "t.h"

/* keymap.library (not in lvo.h) */
#define LVOSetKeyMapDefault (-30)
#define LVOAskKeyMapDefault (-36)
#define LVOMapRawKey (-42)

struct CLI {
	s32 result2;
	u32 setname, commanddir;
	s32 returncode;
	u32 commandname;
	s32 faillevel;
	u32 prompt, standardinput, currentinput, commandfile;
};

static int word(const char **p, char *w, int max)
{
	int n = 0;

	while (**p == ' ')
		(*p)++;
	while (**p && **p != ' ' && **p != '\n' && n < max - 1)
		w[n++] = *(*p)++;
	w[n] = 0;
	return n;
}

static void deep(int n, s32 code)
{
	if (n)
		deep(n - 1, code);
	else
		call(DOSBase, LVOExit, "d1", code);
}

/* a process that puts its own current directory lock down, as it may:
   the lock is its copy, not its creator's */
static volatile int child_done;
static void child(void)
{
	u32 l = call(DOSBase, LVOLock, "d1d2", "SYS:", -2);

	call(DOSBase, LVOUnLock, "d1", call(DOSBase, LVOCurrentDir, "d1", l));
	child_done = 1;
}

static s32 num(const char *s)
{
	s32 v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + *s++ - '0';
	return v;
}

int main(u32 arglen, char *args)
{
	const char *p = args;
	char cmd[32], arg[128];

	word(&p, cmd, sizeof(cmd));
	word(&p, arg, sizeof(arg));
	if (!strcmp(cmd, "cat")) {
		u32 in = call(DOSBase, LVOInput, ""), out = call(DOSBase, LVOOutput, "");
		s32 c;
		/* the argument line comes first, as on AmigaOS */
		while ((c = call(DOSBase, LVOFGetC, "d1", in)) >= 0 && c != '\n')
			;
		while ((c = call(DOSBase, LVOFGetC, "d1", in)) >= 0)
			call(DOSBase, LVOFPutC, "d1d2", out, c);
		return 0;
	}
	if (!strcmp(cmd, "script")) {
		struct CLI *cli = (struct CLI *)(call(DOSBase, LVOCli, "") );
		u32 fh = call(DOSBase, LVOOpen, "d1d2", arg, 1005);
		if (!fh)
			return 20;
		cli->currentinput = fh;
		return 0;
	}
	if (!strcmp(cmd, "cd")) {
		/* System("CD") changes the nested shell's directory, not ours,
		   and leaves our locks alone */
		s32 r = call(DOSBase, LVOSystemTagList, "d1d2", "CD boards", 0);
		u32 l = call(DOSBase, LVOLock, "d1d2", "devices", -2);
		u32 mine = call(DOSBase, LVOLock, "d1d2", "", -2), old, back;
		printf("System CD %ld, still here %ld\n", r, l != 0);
		call(DOSBase, LVOUnLock, "d1", l);
		old = call(DOSBase, LVOCurrentDir, "d1", mine);
		call(DOSBase, LVOSystemTagList, "d1d2", "CD boards", 0);
		back = call(DOSBase, LVOCurrentDir, "d1", old);
		printf("own lock kept %ld\n", back == mine);
		call(DOSBase, LVOUnLock, "d1", mine);
		return 0;
	}
	if (!strcmp(cmd, "proc")) {
		u32 tags[] = { 0x800003e8 + 3, (u32)child,        /* NP_Entry */
			       0x800003e8 + 12, (u32)"child", 0 };  /* NP_Name */
		u32 mine = call(DOSBase, LVOLock, "d1d2", "zorro", -2), old, l;

		old = call(DOSBase, LVOCurrentDir, "d1", mine);
		printf("process %ld\n", call(DOSBase, LVOCreateNewProc, "d1", tags) != 0);
		while (!child_done)
			call(DOSBase, LVODelay, "d1", 1);
		l = call(DOSBase, LVOLock, "d1d2", "devices", -2);
		printf("child done, still here %ld\n", l != 0);
		call(DOSBase, LVOUnLock, "d1", l);
		printf("own lock kept %ld\n", call(DOSBase, LVOCurrentDir, "d1", old) == mine);
		call(DOSBase, LVOUnLock, "d1", mine);
		return 0;
	}
	if (!strcmp(cmd, "exit")) {
		deep(5, num(arg));
		printf("not reached\n");
		return 0;
	}
	if (!strcmp(cmd, "internal")) {
		u32 *seg = (u32 *)call(DOSBase, LVOFindSegment, "d1d2d3", "Echo", 0, 1);
		if (!seg)
			return 20;
		printf("Echo: usecount %ld\n", (s32)seg[1]);
		return call(DOSBase, LVORunCommand, "d1d2d3d4", seg[2], 8192,
			    "from RunCommand\n", 16);
	}
	if (!strcmp(cmd, "vars")) {
		char buf[64];
		s32 n;
		call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", "local", -1, 0);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0);
		printf("local %ld %s\n", n, n >= 0 ? buf : "-");
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0x100);
		printf("global only %ld\n", n);
		call(DOSBase, LVOSetIoErr, "d1", 0);
		n = call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", "x", 0x7fffffff, 0);
		printf("no memory %ld, IoErr %ld\n", n,
		       (s32)call(DOSBase, LVOIoErr, ""));
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0);
		printf("still %ld %s\n", n, n >= 0 ? buf : "-");
		call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", "global\nsecond line", -1, 0x100);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0x100);
		printf("global %ld %s\n", n, buf);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 4, 0x100);
		printf("truncated %ld %s\n", n, buf);
		call(DOSBase, LVOSetVar, "d1d2d3d4", "afvar", 0, 0, 0x100);
		n = call(DOSBase, LVOGetVar, "d1d2d3d4", "afvar", buf, 64, 0x100);
		printf("deleted %ld\n", n);
		return 0;
	}
	if (!strcmp(cmd, "keymap")) {
		/* a keymap with three keys: 0x10 'q' (vanilla), 0x11 a string
		   key, 0x12 a key with shift only */
		static u8 lotypes[64], hitypes[40], locaps[8], lorep[8], hicaps[8], hirep[8];
		static u32 lomap[64], himap[40];
		static u8 str[] = { 3, 4, 3, 7, 'a', 'b', 'c', 'X', 'Y', 'Z' };
		static u32 km[8];
		void *kb = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "keymap.library", 37);
		void *rsrc = (void *)call(SysBase, LVOOpenResource, "a1", "keymap.resource");
		u16 ev[11];
		char buf[16];
		int i, q;
		static const u16 quals[] = { 0, 1, 0x10, 0x11, 8, 4 };
		for (i = 0; i < 64; i++)
			lotypes[i] = 0x80;
		lotypes[0x10] = 7;
		lomap[0x10] = 'Q' << 24 | 0xe5 << 16 | 'Q' << 8 | 'q';
		locaps[2] = 1;
		lotypes[0x11] = 0x41;           /* KCF_STRING | KCF_SHIFT */
		lomap[0x11] = (u32)str;
		lotypes[0x12] = 1;
		lomap[0x12] = '!' << 8 | '1';
		km[0] = (u32)lotypes; km[1] = (u32)lomap; km[2] = (u32)locaps; km[3] = (u32)lorep;
		km[4] = (u32)hitypes; km[5] = (u32)himap; km[6] = (u32)hicaps; km[7] = (u32)hirep;
		printf("keymap.resource %ld\n", rsrc != 0);
		memset(ev, 0, sizeof(ev));
		((u8 *)ev)[4] = 1;              /* IECLASS_RAWKEY */
		for (i = 0x10; i <= 0x12; i++)
			for (q = 0; q < 6; q++) {
				s32 n;
				ev[3] = i;
				ev[4] = quals[q];
				n = call(kb, LVOMapRawKey, "a0a1d1a2", ev, buf, 16, km);
				printf("key $%lx qual $%lx: %ld", (u32)i, (u32)quals[q], n);
				for (int k = 0; k < n; k++)
					printf(" $%lx", (u32)(u8)buf[k]);
				printf("\n");
			}
		call(kb, LVOSetKeyMapDefault, "a0", km);
		printf("default is it %ld\n", call(kb, LVOAskKeyMapDefault, "") == (u32)km);
		ev[3] = 0x12;
		ev[4] = 1;
		i = call(kb, LVOMapRawKey, "a0a1d1a2", ev, buf, 16, 0);
		printf("through the default: %ld $%lx\n", (s32)i, (u32)(u8)buf[0]);
		call(SysBase, LVOCloseLibrary, "a1", kb);
		return 0;
	}
	if (!strcmp(cmd, "pkt")) {
		s32 *dp = (s32 *)call(DOSBase, LVOAllocDosObject, "d1d2", 3, 0);
		u32 me = call(SysBase, LVOFindTask, "a1", 0);
		u32 port = call(DOSBase, LVODeviceProc, "d1", "SYS:");
		s32 *got;
		dp[2] = 1027;                   /* ACTION_IS_FILESYSTEM */
		call(DOSBase, LVOSendPkt, "d1d2d3", dp, port, me + 92);
		got = (s32 *)call(DOSBase, LVOWaitPkt, "");
		printf("filesystem: same packet %ld, res1 %ld\n", got == dp, got[3]);
		dp[2] = 1027;
		port = *(u32 *)(me + 164);      /* pr_ConsoleTask */
		call(DOSBase, LVOSendPkt, "d1d2d3", dp, port, me + 92);
		got = (s32 *)call(DOSBase, LVOWaitPkt, "");
		printf("console: res1 %ld\n", got[3]);
		dp[2] = 12345;
		call(DOSBase, LVOSendPkt, "d1d2d3", dp, port, me + 92);
		got = (s32 *)call(DOSBase, LVOWaitPkt, "");
		printf("unknown: res1 %ld res2 %ld\n", got[3], got[4]);
		/* PutMsg of the packet's message is a packet too */
		dp[2] = 1027;
		dp[1] = me + 92;                /* dp_Port: the reply port */
		call(SysBase, LVOPutMsg, "a0a1", port, dp[0]);
		got = (s32 *)call(DOSBase, LVOWaitPkt, "");
		printf("PutMsg packet: same %ld, res1 %ld\n", got == dp, got[3]);
		/* an ordinary message with a name is not */
		{
			static unsigned short msg[10];  /* struct Message */
			*(u32 *)(msg + 5) = (u32)"named";       /* ln_Name */
			msg[9] = 20;                            /* mn_Length */
			call(SysBase, LVOPutMsg, "a0a1", port, msg);
			printf("named message: queued %ld\n",
			       call(SysBase, LVOGetMsg, "a0", port) == (u32)msg);
		}
		return 0;
	}
	if (!strcmp(cmd, "names")) {
		void *ub = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "utility.library", 39);
		u32 tags[] = { 4000, 1, 4003, 1, 0 };   /* NameSpace, NODUPS */
		u32 prio[] = { 4002, 5, 4001, 16, 0 };  /* Priority, UserSpace */
		u32 ns = call(ub, LVOAllocNamedObjectA, "a0a1", "space", tags);
		u32 a = call(ub, LVOAllocNamedObjectA, "a0a1", "Alpha", 0);
		u32 b = call(ub, LVOAllocNamedObjectA, "a0a1", "beta", prio);
		u32 dup = call(ub, LVOAllocNamedObjectA, "a0a1", "ALPHA", 0);
		u32 o, r1, r2, r3;
		r1 = call(ub, LVOAddNamedObject, "a0a1", ns, a);
		r2 = call(ub, LVOAddNamedObject, "a0a1", ns, b);
		r3 = call(ub, LVOAddNamedObject, "a0a1", ns, dup);
		printf("add %ld %ld %ld (no duplicates)\n", r1, r2, r3);
		printf("user space %ld\n", *(u32 *)b != 0);
		for (o = 0; (o = call(ub, LVOFindNamedObject, "a0a1a2", ns, 0, o)); )
			printf("found %s\n", (char *)call(ub, LVONamedObjectName, "a0", o));
		o = call(ub, LVOFindNamedObject, "a0a1a2", ns, "alpha", 0);
		r1 = call(ub, LVOAttemptRemNamedObject, "a0", a);
		printf("alpha %ld, remove while in use %ld\n", o == a, r1);
		call(ub, LVOReleaseNamedObject, "a0", o);
		call(ub, LVOReleaseNamedObject, "a0", o);
		r1 = call(ub, LVOAttemptRemNamedObject, "a0", a);
		r2 = call(ub, LVOFindNamedObject, "a0a1a2", ns, "alpha", 0);
		printf("remove %ld, then found %ld\n", r1, r2);
		call(ub, LVOFreeNamedObject, "a0", a);
		call(SysBase, LVOCloseLibrary, "a1", ub);
		return 0;
	}
	return 10;
}
