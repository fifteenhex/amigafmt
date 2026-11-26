/*
 * console.device, the library part only (CONU_LIBRARY, unit -1):
 * RawKeyConvert() with the US keymap, what programs use to turn RAWKEY
 * messages into characters.  Console windows (units 0..) are not
 * provided; OpenDevice fails for them.
 */
#include <string.h>
#include "af.h"

/* US keymap: unshifted, shifted; 0 = no character */
static const char km_lo[0x41] =
	"`1234567890-=\\\0000"
	"qwertyuiop[]\000123"
	"asdfghjkl;'\000\000456"
	"\000zxcvbnm,./\000.789";
static const char km_hi[0x41] =
	"~!@#$%^&*()_+|\0000"
	"QWERTYUIOP{}\000123"
	"ASDFGHJKL:\"\000\000456"
	"\000ZXCVBNM<>?\000.789";

static uint32_t keymap_struct(void);
static uint32_t default_km;             /* SetKeyMapDefault()'s, 0 for US */
static int map_key(uint32_t km, int code, int qual, char *out, int max);

static int us_convert(int code, int qual, char *out)
{
	int shift = qual & 3, caps = qual & 4, ctrl = qual & 8, c;

	if (code & 0x80)
		return 0;
	if (code < 0x40) {
		c = shift ? km_hi[code] : km_lo[code];
		if (!c)
			return 0;
		if (caps && !shift && c >= 'a' && c <= 'z')
			c -= 32;
		if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			     c == '@' || c == '[' || c == '\\' || c == ']'))
			c &= 0x1f;
		out[0] = c;
		return 1;
	}
	switch (code) {
	case 0x40: out[0] = ' '; return 1;
	case 0x41: out[0] = 8; return 1;
	case 0x42:
		if (shift) {
			memcpy(out, "\x9bZ", 2);
			return 2;
		}
		out[0] = 9;
		return 1;
	case 0x43: case 0x44: out[0] = 13; return 1;
	case 0x45: out[0] = 27; return 1;
	case 0x46: out[0] = 127; return 1;
	case 0x4a: out[0] = '-'; return 1;
	case 0x4c: case 0x4d: case 0x4e: case 0x4f: {   /* cursor keys */
		static const char *lo[4] = { "\x9b" "A", "\x9b" "B", "\x9b" "C", "\x9b" "D" };
		static const char *hi[4] = { "\x9b" "T", "\x9b" "S", "\x9b" " @", "\x9b" " A" };
		const char *s = (shift ? hi : lo)[code - 0x4c];
		memcpy(out, s, strlen(s));
		return strlen(s);
	}
	case 0x5a: out[0] = '('; return 1;
	case 0x5b: out[0] = ')'; return 1;
	case 0x5c: out[0] = '/'; return 1;
	case 0x5d: out[0] = '*'; return 1;
	case 0x5e: out[0] = '+'; return 1;
	case 0x5f: memcpy(out, "\x9b?~", 3); return 3;
	}
	if (code >= 0x50 && code <= 0x59) {             /* F1..F10 */
		out[0] = (char)0x9b;
		if (shift) {
			out[1] = '1';
			out[2] = '0' + code - 0x50;
			out[3] = '~';
			return 4;
		}
		out[1] = '0' + code - 0x50;
		out[2] = '~';
		return 3;
	}
	return 0;
}

/* the built-in US keymap, or a copy of it (keymap.resource's "usa") */
static int is_us(uint32_t km)
{
	return !km || ((uint32_t *)PTR(km))[1] == ((uint32_t *)PTR(keymap_struct()))[1];
}

/* a raw key in the default keymap (out has room for 8) */
int keymap_convert(int code, int qual, char *out)
{
	if (is_us(default_km))
		return us_convert(code, qual, out);
	return map_key(default_km, code, qual, out, 8);
}

/*
 * A raw key through a struct KeyMap's tables (devices/keymap.h): for
 * keymaps loaded from DEVS:Keymaps.  The qualifiers a key's type names
 * (KCF_SHIFT, KCF_ALT, KCF_CONTROL) pick one of up to four bytes, or one
 * of the strings of a KCF_STRING key; KC_VANILLA's control clears bits
 * 5 and 6.  Dead keys give the character they make on their own.
 */
#define KCF_SHIFT   1
#define KCF_ALT     2
#define KCF_CONTROL 4
#define KCF_DEAD    0x20
#define KCF_STRING  0x40
#define KCF_NOP     0x80
#define KC_VANILLA  7

static int map_key(uint32_t kmp, int code, int qual, char *out, int max)
{
	uint32_t *km = PTR(kmp), data;
	uint8_t *types, *caps, type;
	int hi = code >= 0x40, k = hi ? code - 0x40 : code, idx = 0, bit = 0,
	    want = 0, n = 0, i;

	if (code & 0x80 || code > 0x67 || !km)
		return 0;
	types = PTR(km[hi ? 4 : 0]);
	data = ((uint32_t *)PTR(km[hi ? 5 : 1]))[k];
	caps = PTR(km[hi ? 6 : 2]);
	type = types[k];
	if (type & KCF_NOP)
		return 0;
	if (qual & 3)
		want |= KCF_SHIFT;
	if (qual & 0x30)
		want |= KCF_ALT;
	if (qual & 8)
		want |= KCF_CONTROL;
	if ((qual & 4) && caps && (caps[k >> 3] & (1 << (k & 7))))
		want |= KCF_SHIFT;              /* caps lock on a capsable key */
	/* the pressed qualifiers the key cares about, numbered in order */
	for (i = 0; i < 3; i++) {
		int q = 1 << i;
		if (!(type & q))
			continue;
		if ((type & 7) == KC_VANILLA && q == KCF_CONTROL)
			continue;
		if (want & q)
			idx |= 1 << bit;
		bit++;
	}
	if (type & KCF_STRING) {
		uint8_t *d = PTR(data);
		int len = d[2 * idx], off = d[2 * idx + 1];
		if (len > max)
			len = max;
		memcpy(out, d + off, len);
		return len;
	}
	if (type & KCF_DEAD) {
		uint8_t *d = PTR(data);
		if (d[2 * idx] != 0)            /* a dead key, or one it modifies */
			return d[2 * idx] & 8 ? 0 : (out[0] = d[2 * idx + 1], 1);
		out[0] = d[2 * idx + 1];
		return 1;
	}
	out[n] = data >> (8 * idx);
	if ((type & 7) == KC_VANILLA && (want & KCF_CONTROL))
		out[n] &= 0x9f;
	return out[n] ? 1 : 0;
}

/* struct InputEvent: ie_Class at 4, ie_Code at 6, ie_Qualifier at 8 */
static void c_RawKeyConvert(struct frame *f)
{
	uint8_t *ie = PTR(A(0));
	char buf[8];
	int n;

	if (!ie || ie[4] != 1) {                /* IECLASS_RAWKEY */
		RET(0);
		return;
	}
	n = !is_us(A(2)) ?
	    map_key(A(2), *(uint16_t *)(ie + 6), *(uint16_t *)(ie + 8), buf, 8) :
	    keymap_convert(*(uint16_t *)(ie + 6), *(uint16_t *)(ie + 8), buf);
	if (n > (int32_t)D(1)) {
		RET(-1);
		return;
	}
	memcpy(PTR(A(1)), buf, n);
	RET(n);
}

static void c_CDInputHandler(struct frame *f) { RET(A(0)); }

/*
 * The US keymap as a struct KeyMap, for programs that read the tables
 * (C:KeyShow draws them): km_Lo/HiKeyMapTypes, km_Lo/HiKeyMap (four
 * bytes per key, unshifted in the lowest), Capsable and Repeatable bitmaps.
 */
static uint32_t keymap_struct(void)
{
	static uint32_t km;
	uint8_t *lot, *hit, *locap, *hicap, *lorep, *hirep;
	uint32_t *lomap, *himap, *k;
	int i;

	if (km)
		return km;
	k = amem_alloc(32);
	lot = amem_alloc(64);
	lomap = amem_alloc(64 * 4);
	locap = amem_alloc(8);
	lorep = amem_alloc(8);
	hit = amem_alloc(40);
	himap = amem_alloc(40 * 4);
	hicap = amem_alloc(8);
	hirep = amem_alloc(8);
	for (i = 0; i < 64; i++) {
		int lo = (uint8_t)km_lo[i], hi = (uint8_t)km_hi[i];
		if (!lo) {
			lot[i] = 0x80;          /* KCF_NOP */
			continue;
		}
		lot[i] = 0x03;                  /* KCF_SHIFT | KCF_ALT */
		lomap[i] = (uint32_t)hi << 24 | (uint32_t)lo << 16 | hi << 8 | lo;
		lorep[i >> 3] |= 1 << (i & 7);
		if (lo >= 'a' && lo <= 'z')
			locap[i >> 3] |= 1 << (i & 7);
	}
	for (i = 0; i < 40; i++) {
		char buf[8];
		int n = us_convert(0x40 + i, 0, buf), s;
		if (n == 1) {
			hit[i] = 0x00;          /* KC_NOQUAL */
			himap[i] = (uint8_t)buf[0];
		} else if (n > 1) {
			/* KCF_STRING: (length, offset) then the bytes */
			uint8_t *d = amem_alloc(2 + n);
			d[0] = n;
			d[1] = 2;
			memcpy(d + 2, buf, n);
			hit[i] = 0x40;
			himap[i] = APTR_OF(d);
		} else
			hit[i] = 0x80;
		s = i;
		if (i < 0x10 || (i >= 0x1a && i < 0x20))
			hirep[s >> 3] |= 1 << (s & 7);
	}
	k[0] = APTR_OF(lot);
	k[1] = APTR_OF(lomap);
	k[2] = APTR_OF(locap);
	k[3] = APTR_OF(lorep);
	k[4] = APTR_OF(hit);
	k[5] = APTR_OF(himap);
	k[6] = APTR_OF(hicap);
	k[7] = APTR_OF(hirep);
	km = APTR_OF(k);
	return km;
}

/* a console unit on a window: writes are accepted and dropped, there is
   nothing to read (console windows are not provided) */
int console_io(void *iop)
{
	uint8_t *io = iop;

	if (!lib_console.base || *(uint32_t *)(io + 20) != APTR_OF(lib_console.base))
		return 0;
	switch (*(uint16_t *)(io + 28)) {
	case 3:                                 /* CMD_WRITE */
		*(uint32_t *)(io + 32) = *(uint32_t *)(io + 36);  /* io_Actual */
		io[31] = 0;
		break;
	case 9:                                 /* CD_ASKKEYMAP */
	case 11:                                /* CD_ASKDEFAULTKEYMAP */
		memcpy(PTR(*(uint32_t *)(io + 40)), PTR(keymap_struct()),
		       *(uint32_t *)(io + 36) < 32 ? *(uint32_t *)(io + 36) : 32);
		io[31] = 0;
		break;
	case 10: case 12:                       /* CD_SET(DEFAULT)KEYMAP */
		io[31] = 0;
		break;
	default:
		io[31] = (uint8_t)-3;           /* IOERR_NOCMD */
	}
	return 1;
}
static void c_Nop(struct frame *f) { (void)f; }

/*
 * The private V36 vectors are the console's snip (copy and paste)
 * interface: GetConSnip, SetConSnip, AddConSnipHook, RemConSnipHook
 * (C:ConClip).  There is no console window to select text in, so there
 * is never a snip and a hook is never called.
 */
static void c_Zero(struct frame *f) { RET(0); }

static const struct fdent fd_console[] = {
	{ 30, "BeginIO", "a1" }, { 36, "AbortIO", "a1" },
	{ 42, "CDInputHandler", "a0/a1" }, { 48, "RawKeyConvert", "a0/a1/d1/a2" },
	{ 54, "consolePrivate1", "" }, { 60, "consolePrivate2", "" },
	{ 66, "consolePrivate3", "" }, { 72, "consolePrivate4", "" },
	{ 0, 0, 0 }
};

static const struct libimpl console_impl[] = {
	{ "RawKeyConvert", c_RawKeyConvert },
	{ "CDInputHandler", c_CDInputHandler },
	{ "BeginIO", c_Nop }, { "AbortIO", c_Nop },
	{ "consolePrivate1", c_Zero }, { "consolePrivate2", c_Nop },
	{ "consolePrivate3", c_Nop }, { "consolePrivate4", c_Nop },
	{ NULL, NULL }
};

struct amilib lib_console = {
	.name = "console.device", .version = 40, .revision = 2,
	.fd = fd_console, .impl = console_impl, .possize = 8, .device = 1,
};

/* ---- keymap.library: the same US keymap ---- */

static void k_MapRawKey(struct frame *f)
{
	uint8_t *ie = PTR(A(0));
	char buf[8];
	int n;

	if (!ie || ie[4] != 1) {                /* IECLASS_RAWKEY */
		RET(0);
		return;
	}
	n = !is_us(A(2)) ?
	    map_key(A(2), *(uint16_t *)(ie + 6), *(uint16_t *)(ie + 8), buf, 8) :
	    keymap_convert(*(uint16_t *)(ie + 6), *(uint16_t *)(ie + 8), buf);
	if (n > (int16_t)D(1)) {
		RET(-1);
		return;
	}
	memcpy(PTR(A(1)), buf, n);
	RET(n);
}

/* MapANSI: characters back to (code, qualifier) pairs; 0 for one that
   has no key */
static void k_MapANSI(struct frame *f)
{
	const uint8_t *s = PTR(A(0));
	uint8_t *out = PTR(A(1));
	int32_t n = D(0), max = D(1), i, k = 0;

	for (i = 0; i < n; i++) {
		int code, found = 0;
		for (code = 0; code < 0x60 && !found; code++) {
			static const int quals[] = { 0, 1, 8 };
			for (int q = 0; q < 3; q++) {
				char b[8];
				if (keymap_convert(code, quals[q], b) == 1 &&
				    (uint8_t)b[0] == s[i]) {
					if (k + 2 > max) {
						RET(-1);
						return;
					}
					out[k++] = code;
					out[k++] = quals[q];
					found = 1;
					break;
				}
			}
		}
		if (!found) {
			RET(0);
			return;
		}
	}
	RET(k / 2);
}

static void k_AskKeyMapDefault(struct frame *f)
{
	RET(default_km ? default_km : keymap_struct());
}

static void k_SetKeyMapDefault(struct frame *f)
{
	default_km = A(0);
}

/*
 * keymap.resource: the keymaps in memory, "usa" first (C:SetKeyboard
 * finds or loads one there and makes it the default).
 */
static void keymap_resource(void)
{
	uint8_t *kr = amem_alloc(14 + 14 + 32), *kn = kr + 28;

	((struct Node *)kr)->ln_Name = APTR_OF("keymap.resource");
	((struct Node *)kr)->ln_Type = NT_RESOURCE;
	exec_newlist(kr + 14);                          /* kr_List */
	((struct Node *)kn)->ln_Name = APTR_OF("usa");
	memcpy(kn + 14, PTR(keymap_struct()), 32);      /* kn_KeyMap */
	exec_addtail(kr + 14, kn);
	exec_addtail(&SysBase->ResourceList, kr);
}

static const struct fdent fd_keymap[] = {
	{ 30, "SetKeyMapDefault", "a0" }, { 36, "AskKeyMapDefault", "" },
	{ 42, "MapRawKey", "a0/a1/d1/a2" }, { 48, "MapANSI", "a0/d0/a1/d1/a2" },
	{ 0, 0, 0 }
};

static const struct libimpl keymap_impl[] = {
	{ "SetKeyMapDefault", k_SetKeyMapDefault },
	{ "AskKeyMapDefault", k_AskKeyMapDefault },
	{ "MapRawKey", k_MapRawKey }, { "MapANSI", k_MapANSI },
	{ NULL, NULL }
};

static void keymap_setup(struct Library *b)
{
	(void)b;
	keymap_resource();
}

struct amilib lib_keymap = {
	.name = "keymap.library", .version = 40, .revision = 4,
	.fd = fd_keymap, .impl = keymap_impl, .init = keymap_setup,
};

/* ---- input.device: accepted, but the input stream is wbde's ----
 * OpenDevice succeeds and every command completes (IND_ADDHANDLER,
 * IND_WRITEEVENT, IND_SET*): programs open it to add a handler or to set
 * the key repeat, and refuse to go on when it fails.  Handlers are never
 * called: input reaches programs as IntuiMessages from wbde.
 * PeekQualifier() says no qualifiers.
 */
int input_io(void *iop)
{
	uint8_t *io = iop;

	if (!lib_input.base || *(uint32_t *)(io + 20) != APTR_OF(lib_input.base))
		return 0;
	*(uint32_t *)(io + 32) = 0;             /* io_Actual */
	io[31] = 0;
	return 1;
}

static void in_Zero(struct frame *f) { RET(0); }

static const struct fdent fd_input[] = {
	{ 30, "BeginIO", "a1" }, { 36, "AbortIO", "a1" },
	{ 42, "PeekQualifier", "" },
	{ 0, 0, 0 }
};

static const struct libimpl input_impl[] = {
	{ "PeekQualifier", in_Zero },
	{ "BeginIO", c_Nop }, { "AbortIO", c_Nop },
	{ NULL, NULL }
};

struct amilib lib_input = {
	.name = "input.device", .version = 40, .revision = 1,
	.fd = fd_input, .impl = input_impl, .possize = 8, .device = 1,
};
