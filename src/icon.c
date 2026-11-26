/*
 * icon.library: reading .info files (tool types are what programs want),
 * nothing written back.  The file is struct DiskObject followed by what
 * its pointers pointed at (see wbde's src/icon.c for the layout); the
 * structure is rebuilt in Amiga memory with real pointers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "af.h"
#include "gui.h"

struct ird {
	const uint8_t *p;
	size_t len, at;
	int bad;
};

static const uint8_t *take(struct ird *r, size_t n)
{
	const uint8_t *p;

	if (r->bad || n > r->len - r->at) {
		r->bad = 1;
		return NULL;
	}
	p = r->p + r->at;
	r->at += n;
	return p;
}

static uint32_t take32(struct ird *r)
{
	const uint8_t *p = take(r, 4);
	return p ? (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3] : 0;
}

static char *take_string(struct ird *r)
{
	uint32_t n = take32(r);
	const uint8_t *p;
	char *s;

	if (n > 4096 || !(p = take(r, n)))
		return NULL;
	s = amem_alloc(n + 1);
	memcpy(s, p, n);
	return s;
}

static struct Image *take_image(struct ird *r)
{
	const uint8_t *p = take(r, 20);
	struct Image *im;
	size_t n;

	if (!p)
		return NULL;
	im = amem_alloc(sizeof(*im));
	memcpy(im, p, 20);
	im->NextImage = 0;
	n = (size_t)im->Depth * ((im->Width + 15) / 16) * 2 * im->Height;
	if (im->ImageData) {
		const uint8_t *d = take(r, n);
		uint8_t *c = amem_alloc(n + 2);
		if (d)
			memcpy(c, d, n);
		im->ImageData = APTR_OF(c);
	}
	return im;
}

static struct DiskObject *read_icon(const char *name)
{
	char path[1100], *lp;
	FILE *fp;
	uint8_t *buf;
	long len;
	struct ird r;
	const uint8_t *d;
	struct DiskObject *dob;

	snprintf(path, sizeof(path), "%s.info", name);
	lp = path_to_linux(path, NULL);
	if (!lp || !(fp = fopen(lp, "rb"))) {
		free(lp);
		dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
		return NULL;
	}
	free(lp);
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	rewind(fp);
	buf = malloc(len > 0 ? len : 1);
	if (len <= 0 || fread(buf, 1, len, fp) != (size_t)len) {
		fclose(fp);
		free(buf);
		return NULL;
	}
	fclose(fp);
	r.p = buf;
	r.len = len;
	r.at = 0;
	r.bad = 0;
	d = take(&r, 78);
	if (!d || d[0] != 0xe3 || d[1] != 0x10) {
		free(buf);
		dos_set_ioerr(ERROR_OBJECT_WRONG_TYPE);
		return NULL;
	}
	dob = amem_alloc(sizeof(*dob) + 64);
	memcpy(dob, d, 78);
	if (dob->do_DrawerData) {
		const uint8_t *dd = take(&r, 56);
		uint8_t *c = amem_alloc(64);
		if (dd)
			memcpy(c, dd, 56);
		dob->do_DrawerData = APTR_OF(c);
	}
	dob->do_Gadget.GadgetRender = APTR_OF(take_image(&r));
	if (dob->do_Gadget.SelectRender)
		dob->do_Gadget.SelectRender = APTR_OF(take_image(&r));
	if (dob->do_DefaultTool)
		dob->do_DefaultTool = APTR_OF(take_string(&r));
	if (dob->do_ToolTypes) {
		uint32_t n = take32(&r) / 4, i;
		uint32_t *tt;
		if (n)
			n--;
		if (n > 256)
			n = 0;
		tt = amem_alloc((n + 1) * 4);
		for (i = 0; i < n && !r.bad; i++)
			tt[i] = APTR_OF(take_string(&r));
		dob->do_ToolTypes = APTR_OF(tt);
	}
	if (dob->do_ToolWindow)
		dob->do_ToolWindow = APTR_OF(take_string(&r));
	dob->do_Gadget.NextGadget = 0;
	dob->do_Gadget.GadgetText = 0;
	dob->do_Gadget.SpecialInfo = 0;
	free(buf);
	return dob;
}

static void ic_GetDiskObject(struct frame *f)
{
	RET(read_icon(PTR(A(0))));
}

/* a default project icon when there is no .info */
static struct DiskObject *default_icon(int type)
{
	struct DiskObject *dob = amem_alloc(sizeof(*dob) + 64);

	dob->do_Magic = 0xe310;
	dob->do_Version = 1;
	dob->do_Type = type;
	dob->do_CurrentX = dob->do_CurrentY = 0x80000000;
	dob->do_ToolTypes = APTR_OF(amem_alloc(4));
	dob->do_StackSize = 4096;
	return dob;
}

static void ic_GetDiskObjectNew(struct frame *f)
{
	struct DiskObject *d = read_icon(PTR(A(0)));
	RET(d ? d : default_icon(4));           /* WBPROJECT */
}

static void ic_GetDefDiskObject(struct frame *f)
{
	RET(default_icon(D(0)));
}

static void ic_FreeDiskObject(struct frame *f) { (void)f; }

static void ic_FindToolType(struct frame *f)
{
	uint32_t *tt = PTR(A(0));
	const char *name = PTR(A(1));
	size_t n = strlen(name);

	RET(0);
	for (; tt && *tt; tt++) {
		const char *s = PTR(*tt);
		if (!strncasecmp(s, name, n) && (s[n] == '=' || !s[n])) {
			RET(s[n] == '=' ? s + n + 1 : s + n);
			return;
		}
	}
}

/* "a|b|c" against a value, case-insensitively */
static void ic_MatchToolValue(struct frame *f)
{
	const char *s = PTR(A(0)), *v = PTR(A(1));
	size_t n = strlen(v);

	RET(0);
	while (s && *s) {
		const char *e = strchr(s, '|');
		size_t l = e ? (size_t)(e - s) : strlen(s);
		if (l == n && !strncasecmp(s, v, n)) {
			RET(1);
			return;
		}
		s = e ? e + 1 : NULL;
	}
}

static void ic_PutDiskObject(struct frame *f)
{
	dos_set_ioerr(ERROR_DISK_WRITE_PROTECTED);
	RET(0);
}

static void ic_BumpRevision(struct frame *f)
{
	char *n = PTR(A(0));
	const char *o = PTR(A(1));

	if (!strncmp(o, "copy_", 5))
		snprintf(n, 31, "copy_2_of_%s", o + 5);
	else
		snprintf(n, 31, "copy_of_%s", o);
	RET(A(0));
}

/* FreeList { WORD fl_NumFree; struct List fl_MemList }: memory is kept */
static void ic_AddFreeList(struct frame *f) { RET(1); }
static void ic_Nop(struct frame *f) { (void)f; }

static const struct libimpl icon_impl[] = {
	{ "GetDiskObject", ic_GetDiskObject },
	{ "GetDiskObjectNew", ic_GetDiskObjectNew },
	{ "GetDefDiskObject", ic_GetDefDiskObject },
	{ "FreeDiskObject", ic_FreeDiskObject },
	{ "FindToolType", ic_FindToolType },
	{ "MatchToolValue", ic_MatchToolValue },
	{ "PutDiskObject", ic_PutDiskObject },
	{ "PutDefDiskObject", ic_PutDiskObject },
	{ "DeleteDiskObject", ic_PutDiskObject },
	{ "BumpRevision", ic_BumpRevision },
	{ "AddFreeList", ic_AddFreeList }, { "FreeFreeList", ic_Nop },
	{ NULL, NULL }
};

struct amilib lib_icon = {
	.name = "icon.library", .version = 40, .revision = 1,
	.fd = fd_icon, .impl = icon_impl, .possize = 8,
};

/*
 * workbench.library: AppWindows, AppIcons and AppMenuItems are accepted
 * (wbde has no drag and drop to them yet, so nothing ever arrives).
 * The vector table is written by hand from the 3.1 NDK's wb_lib.fd, which
 * amitools does not have; tools/fdcheck.py checks it.
 */
static void wb_Add(struct frame *f) { RET(amem_alloc(16)); }
static void wb_Remove(struct frame *f)
{
	if (A(0))
		amem_free(PTR(A(0)));
	RET(1);
}
static void wb_Zero(struct frame *f) { RET(0); }

static const struct fdent fd_wb[] = {
	{ 30, "UpdateWorkbench", "a0/a1/d0" }, { 36, "QuoteWorkbench", "d0" },
	{ 42, "StartWorkbench", "d0/d1" },
	{ 48, "AddAppWindowA", "d0/d1/a0/a1/a2" }, { 54, "RemoveAppWindow", "a0" },
	{ 60, "AddAppIconA", "d0/d1/a0/a1/a2/a3/a4" }, { 66, "RemoveAppIcon", "a0" },
	{ 72, "AddAppMenuItemA", "d0/d1/a0/a1/a2" },
	{ 78, "RemoveAppMenuItem", "a0" }, { 84, "WBConfig", "" },
	{ 90, "WBInfo", "a0/a1/a2" },
	{ 0, 0, 0 }
};

/* the Workbench is wbde: it is running when wbde answers */
static void wb_StartWorkbench(struct frame *f)
{
	RET(gui_connect() ? 0 : DOSTRUE);
}

static const struct libimpl wb_impl[] = {
	{ "StartWorkbench", wb_StartWorkbench },
	{ "AddAppWindowA", wb_Add }, { "RemoveAppWindow", wb_Remove },
	{ "AddAppIconA", wb_Add }, { "RemoveAppIcon", wb_Remove },
	{ "AddAppMenuItemA", wb_Add }, { "RemoveAppMenuItem", wb_Remove },
	{ "WBInfo", wb_Zero }, { "UpdateWorkbench", wb_Zero },
	{ NULL, NULL }
};

struct amilib lib_workbench = {
	.name = "workbench.library", .version = 40, .revision = 5,
	.fd = fd_wb, .impl = wb_impl, .possize = 8,
};

/*
 * datatypes.library: present so programs that can use it start; no
 * datatype is known, so objects cannot be made (NewDTObjectA() is NULL
 * with ERROR_OBJECT_NOT_FOUND).  The vector table is written by hand from
 * the 3.1 NDK's datatypes_lib.fd, which amitools does not have.
 */
static void dt_None(struct frame *f)
{
	dos_set_ioerr(ERROR_OBJECT_NOT_FOUND);
	RET(0);
}

static void dt_String(struct frame *f) { RET(""); }

static const struct fdent fd_dt[] = {
	{ 36, "ObtainDataTypeA", "d0/a0/a1" }, { 42, "ReleaseDataType", "a0" },
	{ 48, "NewDTObjectA", "d0/a0" }, { 54, "DisposeDTObject", "a0" },
	{ 60, "SetDTAttrsA", "a0/a1/a2/a3" }, { 66, "GetDTAttrsA", "a0/a2" },
	{ 72, "AddDTObject", "a0/a1/a2/d0" }, { 78, "RefreshDTObjectA", "a0/a1/a2/a3" },
	{ 84, "DoAsyncLayout", "a0/a1" }, { 90, "DoDTMethodA", "a0/a1/a2/a3" },
	{ 96, "RemoveDTObject", "a0/a1" }, { 102, "GetDTMethods", "a0" },
	{ 108, "GetDTTriggerMethods", "a0" }, { 114, "PrintDTObjectA", "a0/a1/a2/a3" },
	{ 138, "GetDTString", "d0" },
	{ 0, 0, 0 }
};

static const struct libimpl dt_impl[] = {
	{ "ObtainDataTypeA", dt_None }, { "ReleaseDataType", wb_Zero },
	{ "NewDTObjectA", dt_None }, { "DisposeDTObject", wb_Zero },
	{ "SetDTAttrsA", wb_Zero }, { "GetDTAttrsA", wb_Zero },
	{ "AddDTObject", wb_Zero }, { "RefreshDTObjectA", wb_Zero },
	{ "DoDTMethodA", wb_Zero }, { "RemoveDTObject", wb_Zero },
	{ "GetDTMethods", wb_Zero }, { "GetDTTriggerMethods", wb_Zero },
	{ "GetDTString", dt_String },
	{ NULL, NULL }
};

struct amilib lib_datatypes = {
	.name = "datatypes.library", .version = 40, .revision = 1,
	.fd = fd_dt, .impl = dt_impl, .possize = 8,
};
