/*
 * expansion.library: the list of AutoConfig boards.
 *
 * Linux has configured the boards already and knows them: on an Amiga
 * each Zorro device has its ConfigDev, as AmigaOS built it and handed it
 * to the kernel, in /sys/bus/zorro/devices/<n>/config (or the older
 * /proc/bus/zorro/<nn>).  Those become the board list.  $AMIGAFMT_ZORRO
 * replaces it: "none" for no boards, a directory to read ConfigDevs from
 * (either of those layouts, or plain ConfigDev files), or a list of
 * boards as manufacturer/product@address/size, separated by commas.
 *
 * Nothing here touches the hardware: the calls that would configure a
 * board or read its expansion ROM fail, and the expansion memory
 * allocator has no space to give.  Mounting (AddDosNode) is refused as
 * well, since there are no handlers to run a device.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "af.h"

#define EB_BOARDS 60            /* eb_Private05: the ConfigDev list */
#define EB_MOUNTLIST 74

static struct Library *ExpansionBase;
static struct CurrentBinding binding;

static struct List *boards(void)
{
	return (struct List *)((uint8_t *)ExpansionBase + EB_BOARDS);
}

/* a ConfigDev from a file of sizeof(struct ConfigDev) big-endian bytes */
static void load_one(const char *path)
{
	struct ConfigDev *cd;
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return;
	cd = amem_alloc(sizeof(*cd));
	n = read(fd, cd, sizeof(*cd));
	close(fd);
	if (n != sizeof(*cd) || !cd->cd_Rom.er_Manufacturer) {
		amem_free(cd);
		return;
	}
	cd->cd_Node.ln_Succ = cd->cd_Node.ln_Pred = 0;
	cd->cd_Node.ln_Name = 0;
	cd->cd_Driver = cd->cd_NextCD = 0;
	exec_addtail(boards(), cd);
}

static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* every entry of dir, in name order (the slot order), as dir/name[/sub] */
static int load_dir(const char *dir, const char *sub)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char *names[64], path[512];
	int n = 0, i;

	if (!d)
		return 0;
	while ((de = readdir(d)) && n < 64)
		if (de->d_name[0] != '.' && strcmp(de->d_name, "devices"))
			names[n++] = strdup(de->d_name);
	closedir(d);
	qsort(names, n, sizeof(char *), cmp_names);
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s%s%s", dir, names[i],
			 sub ? "/" : "", sub ? sub : "");
		if (!access(path, R_OK))
			load_one(path);
		free(names[i]);
	}
	return 1;
}

/* is dir/de a directory: stat() it where d_type does not say */
static int is_dir(const char *dir, struct dirent *de)
{
	char path[512];
	struct stat st;

	if (de->d_type != DT_UNKNOWN && de->d_type != DT_LNK)
		return de->d_type == DT_DIR;
	snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int has_subdirs(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int r = 0;

	while (d && !r && (de = readdir(d)))
		if (de->d_name[0] != '.' && is_dir(dir, de))
			r = 1;
	if (d)
		closedir(d);
	return r;
}

static int no_boards(void)
{
	return boards()->lh_Head == APTR_OF(&boards()->lh_Tail);
}

/* a board from the AMIGAFMT_ZORRO list: what its ConfigDev would say */
static void make_board(uint32_t manu, uint32_t prod, uint32_t addr,
		       uint32_t size)
{
	struct ConfigDev *cd = amem_alloc(sizeof(*cd));
	int code = 0;

	cd->cd_Rom.er_Manufacturer = manu;
	cd->cd_Rom.er_Product = prod;
	if (addr >= 0x01000000 || size > 0x800000) {
		/* Zorro III: 64K..8M as Zorro II's table, then the extended
		   one, 16M..1G */
		if (size >= 0x1000000) {
			cd->cd_Rom.er_Flags = 0x30;     /* ZORRO_III|EXTENDED */
			for (code = 0; code < 6 && (0x1000000u << code) < size; code++)
				;
		} else {
			cd->cd_Rom.er_Flags = 0x10;
			for (code = 1; code < 8 && (0x10000u << (code - 1)) < size; code++)
				;
			code &= 7;
		}
		cd->cd_Rom.er_Type = 0x80 | code;
	} else {
		for (code = 1; code < 8 && (0x10000u << (code - 1)) < size; code++)
			;
		cd->cd_Rom.er_Type = 0xc0 | (code & 7);
	}
	cd->cd_BoardAddr = addr;
	cd->cd_BoardSize = size;
	cd->cd_SlotAddr = addr >> 16;
	cd->cd_SlotSize = size >> 16;
	exec_addtail(boards(), cd);
}

static void parse_boards(const char *spec)
{
	const char *p = spec;

	while (*p) {
		char *e;
		uint32_t manu, prod, addr, size;

		manu = strtoul(p, &e, 0);
		if (*e != '/')
			break;
		prod = strtoul(e + 1, &e, 0);
		if (*e != '@')
			break;
		addr = strtoul(e + 1, &e, 0);
		if (*e != '/')
			break;
		size = strtoul(e + 1, &e, 0);
		make_board(manu, prod, addr, size);
		p = e;
		if (*p == ',')
			p++;
		else if (*p)
			break;
	}
	if (*p)
		af_warn("AMIGAFMT_ZORRO: cannot read the board list at \"%s\"", p);
}

static void load_boards(void)
{
	const char *z = getenv("AMIGAFMT_ZORRO");
	char path[512];

	if (z) {
		if (*z && *z != '/' && *z != '.' && strcmp(z, "none")) {
			parse_boards(z);
			return;
		}
		if (strcmp(z, "none")) {
			snprintf(path, sizeof(path), "%s/devices", z);
			if (has_subdirs(path))
				load_dir(path, "config");
			else
				load_dir(z, has_subdirs(z) ? "config" : NULL);
		}
		return;
	}
	load_dir("/sys/bus/zorro/devices", "config");
	if (no_boards() && machine.amiga)
		load_dir("/proc/bus/zorro", NULL);
}

static void x_FindConfigDev(struct frame *f)
{
	struct ConfigDev *cd = PTR(A(0));
	int32_t manu = D(0), prod = D(1);

	cd = PTR(cd ? cd->cd_Node.ln_Succ : boards()->lh_Head);
	for (; cd && cd->cd_Node.ln_Succ; cd = PTR(cd->cd_Node.ln_Succ))
		if ((manu == -1 || cd->cd_Rom.er_Manufacturer == (uint16_t)manu) &&
		    (prod == -1 || cd->cd_Rom.er_Product == (uint8_t)prod)) {
			RET(cd);
			return;
		}
	RET(0);
}

static void x_AddConfigDev(struct frame *f)
{
	if (A(0))
		exec_addtail(boards(), PTR(A(0)));
}

static void x_RemConfigDev(struct frame *f)
{
	if (A(0))
		exec_remove(PTR(A(0)));
}

static void x_AllocConfigDev(struct frame *f)
{
	RET(amem_alloc(sizeof(struct ConfigDev)));
}

static void x_FreeConfigDev(struct frame *f)
{
	if (A(0))
		amem_free(PTR(A(0)));
}

static void x_Nop(struct frame *f) { (void)f; }

static void x_SetCurrentBinding(struct frame *f)
{
	uint32_t n = D(0) & 0xffff;

	if (n > sizeof(binding))
		n = sizeof(binding);
	memset(&binding, 0, sizeof(binding));
	memcpy(&binding, PTR(A(0)), n);
}

static void x_GetCurrentBinding(struct frame *f)
{
	uint32_t n = D(0) & 0xffff;

	if (n > sizeof(binding))
		n = sizeof(binding);
	memcpy(PTR(A(0)), &binding, n);
	RET(sizeof(binding));
}

/*
 * MakeDosNode: a DeviceNode, a FileSysStartupMsg, the environment vector
 * and the two names as BCPL strings, all longword aligned; the parameter
 * packet is the names, unit, flags, then the environment from its size
 * on (libraries/filehandler.h).
 */
static void x_MakeDosNode(struct frame *f)
{
	uint32_t *pp = PTR(A(0));
	const char *dosname = PTR(pp[0]), *devname = PTR(pp[1]);
	uint32_t envn = pp[4] + 1, sz;
	uint8_t *m, *dn, *fssm, *env, *n1, *n2;

	sz = 44 + 16 + envn * 4 + 2 * 256;
	m = amem_alloc(sz);
	if (!m) {
		RET(0);
		return;
	}
	dn = m;                         /* struct DeviceNode, 44 bytes */
	fssm = dn + 44;                 /* struct FileSysStartupMsg, 16 */
	env = fssm + 16;
	n1 = env + envn * 4;
	n2 = n1 + 256;
	memcpy(env, pp + 4, envn * 4);
	n1[0] = strlen(dosname) > 255 ? 255 : strlen(dosname);
	memcpy(n1 + 1, dosname, n1[0]);
	n2[0] = strlen(devname) > 255 ? 255 : strlen(devname);
	memcpy(n2 + 1, devname, n2[0]);
	n2[n2[0] + 1] = 0;              /* also NUL-terminated, as the ROM's */
	*(uint32_t *)(fssm + 0) = pp[2];                /* fssm_Unit */
	*(BPTR *)(fssm + 4) = MKBADDR(n2);              /* fssm_Device */
	*(BPTR *)(fssm + 8) = MKBADDR(env);             /* fssm_Environ */
	*(uint32_t *)(fssm + 12) = pp[3];               /* fssm_Flags */
	*(int32_t *)(dn + 4) = 0;                       /* DLT_DEVICE */
	*(int32_t *)(dn + 20) = 600;                    /* dn_StackSize */
	*(int32_t *)(dn + 24) = 10;                     /* dn_Priority */
	*(BPTR *)(dn + 28) = MKBADDR(fssm);             /* dn_Startup */
	*(int32_t *)(dn + 36) = -1;                     /* dn_GlobalVec */
	*(BPTR *)(dn + 40) = MKBADDR(n1);               /* dn_Name */
	RET(dn);
}

/* there are no handlers to run a device: mounting is refused */
static void x_AddDosNode(struct frame *f)
{
	RET(0);
}

/* the hardware side: nothing is configured, read or allocated here */
static void x_ReadExpansionRom(struct frame *f) { RET(-1); }    /* no board */
static void x_ReadExpansionByte(struct frame *f) { RET(-1); }
static void x_ConfigBoard(struct frame *f) { RET(41); }         /* EE_NOEXPANSION */
static void x_NoSlot(struct frame *f) { RET(-1); }
static void x_Zero(struct frame *f) { RET(0); }

static const struct fdent fd_expansion[] = {
	{ 30, "AddConfigDev", "a0" },
	{ 36, "AddBootNode", "d0/d1/a0/a1" },
	{ 42, "AllocBoardMem", "d0" },
	{ 48, "AllocConfigDev", "" },
	{ 54, "AllocExpansionMem", "d0/d1" },
	{ 60, "ConfigBoard", "a0/a1" },
	{ 66, "ConfigChain", "a0" },
	{ 72, "FindConfigDev", "a0/d0/d1" },
	{ 78, "FreeBoardMem", "d0/d1" },
	{ 84, "FreeConfigDev", "a0" },
	{ 90, "FreeExpansionMem", "d0/d1" },
	{ 96, "ReadExpansionByte", "a0/d0" },
	{ 102, "ReadExpansionRom", "a0/a1" },
	{ 108, "RemConfigDev", "a0" },
	{ 114, "WriteExpansionByte", "a0/d0/d1" },
	{ 120, "ObtainConfigBinding", "" },
	{ 126, "ReleaseConfigBinding", "" },
	{ 132, "SetCurrentBinding", "a0/d0" },
	{ 138, "GetCurrentBinding", "a0/d0" },
	{ 144, "MakeDosNode", "a0" },
	{ 150, "AddDosNode", "d0/d1/a0" },
	{ 0, 0, 0 }
};

static const struct libimpl expansion_impl[] = {
	{ "AddConfigDev", x_AddConfigDev }, { "RemConfigDev", x_RemConfigDev },
	{ "AllocConfigDev", x_AllocConfigDev },
	{ "FreeConfigDev", x_FreeConfigDev },
	{ "FindConfigDev", x_FindConfigDev },
	{ "ObtainConfigBinding", x_Nop }, { "ReleaseConfigBinding", x_Nop },
	{ "SetCurrentBinding", x_SetCurrentBinding },
	{ "GetCurrentBinding", x_GetCurrentBinding },
	{ "MakeDosNode", x_MakeDosNode },
	{ "AddDosNode", x_AddDosNode }, { "AddBootNode", x_AddDosNode },
	{ "ReadExpansionRom", x_ReadExpansionRom },
	{ "ReadExpansionByte", x_ReadExpansionByte },
	{ "WriteExpansionByte", x_Zero },
	{ "ConfigBoard", x_ConfigBoard }, { "ConfigChain", x_ConfigBoard },
	{ "AllocBoardMem", x_NoSlot }, { "AllocExpansionMem", x_NoSlot },
	{ "FreeBoardMem", x_Nop }, { "FreeExpansionMem", x_Nop },
	{ NULL, NULL }
};

static void expansion_setup(struct Library *b)
{
	ExpansionBase = b;
	exec_newlist(boards());
	exec_newlist((uint8_t *)b + EB_MOUNTLIST);
	load_boards();
}

struct amilib lib_expansion = {
	.name = "expansion.library", .version = 40, .revision = 2,
	.fd = fd_expansion, .impl = expansion_impl,
	.possize = 88 - sizeof(struct Library),
	.init = expansion_setup,
};
