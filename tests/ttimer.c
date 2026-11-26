/* timer.device: DoIO, SendIO + Wait, CheckIO, AbortIO, time arithmetic */
#include "t.h"

struct MsgPort { u8 node[14]; u8 flags, sigbit; u32 sigtask; u8 list[14]; };
struct timereq {
	u8 node[14]; struct MsgPort *reply; u16 len;
	void *device; u32 unit; u16 command; u8 flags; s8 error;
	u32 secs, micro;
};
struct tv { u32 secs, micro; };

static void *tbase;

static u32 ms(struct tv *a, struct tv *b)
{
	return (b->secs - a->secs) * 1000 + (s32)(b->micro - a->micro) / 1000;
}

int main(u32 arglen, char *args)
{
	struct MsgPort *mp = (void *)call(SysBase, LVOCreateMsgPort, "");
	struct timereq *tr = (void *)call(SysBase, LVOCreateIORequest, "a0d0",
					  mp, sizeof(struct timereq));
	struct timereq *t2 = (void *)call(SysBase, LVOCreateIORequest, "a0d0",
					  mp, sizeof(struct timereq));
	struct tv a, b, c;
	u32 sig;

	if (call(SysBase, LVOOpenDevice, "a0d0a1d1", "timer.device", 0, tr, 0)) {
		printf("OpenDevice failed\n");
		return 20;
	}
	*t2 = *tr;
	tbase = tr->device;
	printf("nonesuch.device: %ld\n", call(SysBase, LVOOpenDevice,
	       "a0d0a1d1", "nonesuch.device", 0, t2, 0));
	*t2 = *tr;

	call(tbase, LVOGetSysTime, "a0", &a);
	tr->command = 9;                        /* TR_ADDREQUEST */
	tr->secs = 0;
	tr->micro = 300000;
	printf("DoIO = %ld\n", call(SysBase, LVODoIO, "a1", tr));
	call(tbase, LVOGetSysTime, "a0", &b);
	printf("DoIO waited 300..800 ms: %ld\n",
	       ms(&a, &b) >= 300 && ms(&a, &b) < 800);

	tr->micro = 200000;
	call(SysBase, LVOSendIO, "a1", tr);
	printf("CheckIO at once: %s\n",
	       call(SysBase, LVOCheckIO, "a1", tr) ? "done" : "pending");
	sig = call(SysBase, LVOWait, "d0", 1u << mp->sigbit);
	call(tbase, LVOGetSysTime, "a0", &c);
	printf("Wait got the port signal: %ld, 200..700 ms: %ld\n",
	       sig == 1u << mp->sigbit, ms(&b, &c) >= 200 && ms(&b, &c) < 700);
	printf("GetMsg is the request: %ld\n",
	       call(SysBase, LVOGetMsg, "a0", mp) == (u32)tr);

	t2->command = 9;
	t2->secs = 10;
	t2->micro = 0;
	call(SysBase, LVOSendIO, "a1", t2);
	call(SysBase, LVOAbortIO, "a1", t2);
	printf("WaitIO after AbortIO: %ld\n", call(SysBase, LVOWaitIO, "a1", t2));

	a.secs = 1; a.micro = 900000; b.secs = 2; b.micro = 200000;
	call(tbase, LVOAddTime, "a0a1", &a, &b);
	printf("AddTime: %ld.%06ld\n", a.secs, a.micro);
	call(tbase, LVOSubTime, "a0a1", &a, &b);
	printf("SubTime: %ld.%06ld\n", a.secs, a.micro);
	printf("CmpTime: %ld %ld %ld\n", call(tbase, LVOCmpTime, "a0a1", &a, &b),
	       call(tbase, LVOCmpTime, "a0a1", &b, &a),
	       call(tbase, LVOCmpTime, "a0a1", &a, &a));
	printf("ReadEClock freq %ld\n", call(tbase, LVOReadEClock, "a0", &a));
	call(SysBase, LVOCloseDevice, "a1", tr);
	call(SysBase, LVODeleteIORequest, "a0", tr);
	call(SysBase, LVODeleteIORequest, "a0", t2);
	call(SysBase, LVODeleteMsgPort, "a0", mp);
	return 0;
}
