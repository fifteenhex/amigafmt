/*
 * A custom screen on wbde: tscreen [mode [seconds]]
 *   mode: lores (NTSC 320x200x5, the default), hires (640x200x4),
 *         pal (PAL 320x256x5), laced (NTSC 640x400x3)
 * Opens the screen with its own palette (SA_Colors32, then LoadRGB4 and
 * SetRGB32 on its ViewPort), draws a colour bar per pen on the screen's
 * own RastPort, opens a backdrop borderless window and a window with a
 * close gadget on it, prints what the Screen structure says, then waits
 * for the close gadget or the seconds to pass.  Needs wbde: it is driven
 * by hand or by a script, not by tests/run.sh.
 */
#include "t.h"
#include "lvo_gui.h"

#define TAG_DONE 0
#define SA_Dummy (0x80000000 + 32)
#define SA_Width (SA_Dummy + 3)
#define SA_Height (SA_Dummy + 4)
#define SA_Depth (SA_Dummy + 5)
#define SA_Title (SA_Dummy + 8)
#define SA_DisplayID (SA_Dummy + 0x12)
#define SA_Pens (SA_Dummy + 0x1a)
#define SA_Colors32 (SA_Dummy + 0x23)
#define WA_Dummy (0x80000000 + 99)
#define WA_Left (WA_Dummy + 1)
#define WA_Top (WA_Dummy + 2)
#define WA_Width (WA_Dummy + 3)
#define WA_Height (WA_Dummy + 4)
#define WA_IDCMP (WA_Dummy + 7)
#define WA_Title (WA_Dummy + 11)
#define WA_CustomScreen (WA_Dummy + 13)
#define WA_CloseGadget (WA_Dummy + 0x21)
#define WA_DragBar (WA_Dummy + 0x1f)
#define WA_DepthGadget (WA_Dummy + 0x20)
#define WA_Backdrop (WA_Dummy + 0x22)
#define WA_Borderless (WA_Dummy + 0x25)
#define WA_Activate (WA_Dummy + 0x26)
#define IDCMP_CLOSEWINDOW 0x200

static void *IntuitionBase, *GfxBase;

/* the bits of struct Screen and Window this looks at */
#define SCR_W(s) (*(s16 *)((u8 *)(s) + 12))
#define SCR_H(s) (*(s16 *)((u8 *)(s) + 14))
#define SCR_VP(s) ((u8 *)(s) + 44)
#define SCR_RP(s) ((u8 *)(s) + 84)
#define SCR_BM(s) ((u8 *)(s) + 184)
#define WIN_RP(w) (*(void **)((u8 *)(w) + 50))
#define WIN_PORT(w) (*(u8 **)((u8 *)(w) + 86))

static int streq(const char *a, const char *b)
{
	return !strcmp(a, b);
}

int main(u32 arglen, char *args)
{
	static u32 cols[1 + 32 * 3 + 1];
	static const u16 pens[] = { 0xffff };
	char mode[16] = "lores";
	u32 tags[16], wt[24], id = 0x11000, w = 320, h = 200, d = 5, secs = 5, i, n;
	void *s, *bd, *win;
	u8 *rp;

	/* arguments: a word and a number, by hand */
	for (i = 0; i < arglen && args[i] == ' '; i++)
		;
	for (n = 0; i < arglen && args[i] > ' ' && n < 15; i++)
		mode[n++] = args[i];
	if (n)
		mode[n] = 0;
	for (; i < arglen && args[i] == ' '; i++)
		;
	if (i < arglen && args[i] >= '0' && args[i] <= '9')
		for (secs = 0; i < arglen && args[i] >= '0' && args[i] <= '9'; i++)
			secs = secs * 10 + args[i] - '0';
	if (streq(mode, "hires"))
		id = 0x19000, w = 640, h = 200, d = 4;
	else if (streq(mode, "pal"))
		id = 0x21000, w = 320, h = 256, d = 5;
	else if (streq(mode, "laced"))
		id = 0x19004, w = 640, h = 400, d = 3;

	IntuitionBase = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "intuition.library", 39);
	GfxBase = (void *)call(SysBase, LVOOpenLibrary, "a1d0", "graphics.library", 39);
	/* a grey ramp to start with: SA_Colors32 */
	n = 1 << d;
	cols[0] = n << 16 | 0;
	for (i = 0; i < n; i++)
		cols[1 + 3 * i] = cols[2 + 3 * i] = cols[3 + 3 * i] = (i * 255 / (n - 1)) * 0x01010101u;
	cols[1 + 3 * n] = 0;
	i = 0;
	tags[i++] = SA_Width; tags[i++] = w;
	tags[i++] = SA_Height; tags[i++] = h;
	tags[i++] = SA_Depth; tags[i++] = d;
	tags[i++] = SA_DisplayID; tags[i++] = id;
	tags[i++] = SA_Title; tags[i++] = (u32)"tscreen: a custom screen";
	tags[i++] = SA_Pens; tags[i++] = (u32)pens;
	tags[i++] = SA_Colors32; tags[i++] = (u32)cols;
	tags[i++] = TAG_DONE;
	s = (void *)call(IntuitionBase, LVOOpenScreenTagList, "a0a1", 0, tags);
	if (!s) {
		printf("OpenScreenTagList failed\n");
		return 20;
	}
	printf("screen %ldx%ld depth %ld modes $%lx modeid $%lx\n", (s32)SCR_W(s), (s32)SCR_H(s),
	       (s32)SCR_BM(s)[5], (u32)*(u16 *)(SCR_VP(s) + 32),
	       call(GfxBase, LVOGetVPModeID, "a0", SCR_VP(s)));
	/* colours 1..3 the Amiga way (LoadRGB4), 4 with SetRGB32 */
	{
		static const u16 rgb4[4] = { 0x000, 0xf00, 0x0f0, 0x00f };
		call(GfxBase, LVOLoadRGB4, "a0a1d0", SCR_VP(s), rgb4, 4);
		call(GfxBase, LVOSetRGB32, "a0d0d1d2d3", SCR_VP(s), 4, 0xffffffff, 0xffffffff, 0);
	}
	printf("colour 4 reads back $%lx\n",
	       call(GfxBase, LVOGetRGB4, "a0d0", *(void **)(SCR_VP(s) + 4), 4));
	/* a bar per pen on the screen's own RastPort, under the windows */
	rp = SCR_RP(s);
	for (i = 0; i < n; i++) {
		call(GfxBase, LVOSetAPen, "a1d0", rp, i);
		call(GfxBase, LVORectFill, "a1d0d1d2d3", rp, i * w / n, h / 2, (i + 1) * w / n - 1, h - 1);
	}
	call(GfxBase, LVOSetAPen, "a1d0", rp, 1);
	call(GfxBase, LVOMove, "a1d0d1", rp, 8, h / 2 - 6);
	call(GfxBase, LVOText, "a1a0d0", rp, "the screen's RastPort", 21);

	/* a backdrop borderless window over the top part, and a real window */
	i = 0;
	wt[i++] = WA_CustomScreen; wt[i++] = (u32)s;
	wt[i++] = WA_Left; wt[i++] = 0;
	wt[i++] = WA_Top; wt[i++] = 12;
	wt[i++] = WA_Width; wt[i++] = w / 2;
	wt[i++] = WA_Height; wt[i++] = h / 4;
	wt[i++] = WA_Backdrop; wt[i++] = 1;
	wt[i++] = WA_Borderless; wt[i++] = 1;
	wt[i++] = TAG_DONE;
	bd = (void *)call(IntuitionBase, LVOOpenWindowTagList, "a0a1", 0, wt);
	if (bd) {
		call(GfxBase, LVOSetAPen, "a1d0", WIN_RP(bd), 2);
		call(GfxBase, LVORectFill, "a1d0d1d2d3", WIN_RP(bd), 0, 0, w / 2 - 1, h / 4 - 1);
		call(GfxBase, LVOSetAPen, "a1d0", WIN_RP(bd), 1);
		call(GfxBase, LVOMove, "a1d0d1", WIN_RP(bd), 4, 10);
		call(GfxBase, LVOText, "a1a0d0", WIN_RP(bd), "backdrop", 8);
		call(GfxBase, LVOSetAPen, "a1d0", WIN_RP(bd), 3);
		call(GfxBase, LVOMove, "a1d0d1", WIN_RP(bd), 4, 14);
		call(GfxBase, LVODraw, "a1d0d1", WIN_RP(bd), w / 2 - 4, h / 4 - 2);
	}
	i = 0;
	wt[i++] = WA_CustomScreen; wt[i++] = (u32)s;
	wt[i++] = WA_Left; wt[i++] = w / 2 - 20;
	wt[i++] = WA_Top; wt[i++] = 20;
	wt[i++] = WA_Width; wt[i++] = w / 2;
	wt[i++] = WA_Height; wt[i++] = h / 3;
	wt[i++] = WA_Title; wt[i++] = (u32)"on the screen";
	wt[i++] = WA_CloseGadget; wt[i++] = 1;
	wt[i++] = WA_DragBar; wt[i++] = 1;
	wt[i++] = WA_DepthGadget; wt[i++] = 1;
	wt[i++] = WA_Activate; wt[i++] = 1;
	wt[i++] = WA_IDCMP; wt[i++] = IDCMP_CLOSEWINDOW;
	wt[i++] = TAG_DONE;
	win = (void *)call(IntuitionBase, LVOOpenWindowTagList, "a0a1", 0, wt);
	printf("windows %s %s\n", bd ? "backdrop" : "-", win ? "titled" : "-");
	if (win) {
		/* a crosshair pointer while the window is active (SetPointer) */
		static const u16 cross[2 + 2 * 7 + 2] = {
			0, 0,
			0x1000, 0, 0x1000, 0, 0x1000, 0, 0xee00, 0x1000,
			0x1000, 0, 0x1000, 0, 0x1000, 0,
			0, 0,
		};
		call(IntuitionBase, LVOSetPointer, "a0a1d0d1d2d3", win, cross, 7, 16, -3, -3);
		call(GfxBase, LVOSetAPen, "a1d0", WIN_RP(win), 3);
		call(GfxBase, LVOMove, "a1d0d1", WIN_RP(win), 10, 24);
		call(GfxBase, LVOText, "a1a0d0", WIN_RP(win), "blue on grey", 12);
	}
	/* until the close gadget, or the time is up */
	for (i = 0; i < secs * 10; i++) {
		u8 *port = win ? WIN_PORT(win) : 0;
		void *m = port ? (void *)call(SysBase, LVOGetMsg, "a0", port) : 0;
		if (m) {
			call(SysBase, LVOReplyMsg, "a1", m);
			printf("close gadget\n");
			break;
		}
		call(DOSBase, LVODelay, "d1", 5);
	}
	if (win)
		call(IntuitionBase, LVOCloseWindow, "a0", win);
	if (bd)
		call(IntuitionBase, LVOCloseWindow, "a0", bd);
	printf("CloseScreen %ld\n", call(IntuitionBase, LVOCloseScreen, "a0", s));
	return 0;
}
