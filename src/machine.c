/*
 * The machine amigafmt says it is running on.
 *
 * On an Amiga, Linux knows: /proc/hardware has the chipset, PAL or NTSC,
 * the E clock, the power supply frequency and the amount of chip RAM, and
 * /proc/iomem the fast RAM regions.  Elsewhere the answers are those of
 * an NTSC AGA machine, which is what the custom chip emulation and the
 * display database already claim.
 *
 * Exec's memory list is built from the same figures: a MemHeader per
 * region, each at the start of a range of address space reserved for it
 * (PROT_NONE but for the header's own page), so that the header address,
 * mh_Lower and mh_Upper describe a region of the right size, as on
 * AmigaOS, and at the region's physical address when that is free in
 * this process.  Nothing is ever allocated from them: AllocMem has its
 * own allocator.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include "af.h"

struct machine machine;

static void read_hardware(void)
{
	FILE *fp = fopen("/proc/hardware", "r");
	char line[256];

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *v = strchr(line, ':');

		if (v)
			for (v++; *v == ' ' || *v == '\t'; v++)
				;
		if (!strncmp(line, "Model:", 6) && v && !strncmp(v, "Amiga", 5)) {
			machine.amiga = 1;
			snprintf(machine.model, sizeof(machine.model), "%.*s",
				 (int)strcspn(v, "\n"), v);
		} else if (!strncmp(line, "Chip RAM:", 9) && v)
			machine.chip_size = strtoul(v, NULL, 10) << 10;
		else if (!strncmp(line, "PS Freq:", 8) && v)
			machine.psfreq = atoi(v);
		else if (!strncmp(line, "EClock Freq:", 12) && v)
			machine.eclock = strtoul(v, NULL, 10);
		else if (!strncmp(line, "Graphics:", 9) && v)
			machine.chipset = !strncmp(v, "AGA", 3) ? CHIPSET_AGA :
				!strncmp(v, "ECS", 3) ? CHIPSET_ECS : CHIPSET_OCS;
		else if (line[0] == '\t' && strstr(line, "Agnus 837") &&
			 strstr(line, "PAL") && !strstr(line, "NTSC"))
			machine.pal = 1;        /* "Fat Hires PAL Agnus 8372" */
		else if (line[0] == '\t' && strstr(line, "PAL Alice"))
			machine.pal = 1;
		else if (line[0] == '\t' && strstr(line, "PAL Agnus"))
			machine.pal = 1;
		else if (line[0] == '\t' && strstr(line, "Denise 8373"))
			machine.hr_denise = 1;
	}
	fclose(fp);
}

/* the fast RAM regions Linux was given: top-level "... RAM" in iomem */
static void read_iomem(void)
{
	FILE *fp = fopen("/proc/iomem", "r");
	char line[256];

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp) && machine.nregions < MACHINE_REGIONS) {
		unsigned long lo, hi;
		char *name;

		if (line[0] == ' ' || sscanf(line, "%lx-%lx", &lo, &hi) != 2)
			continue;
		name = strstr(line, " : ");
		if (!name || !strstr(name, "RAM") || strstr(name, "Chip"))
			continue;
		if (hi <= lo)
			continue;               /* addresses hidden from non-root */
		machine.region[machine.nregions].base = lo;
		machine.region[machine.nregions++].size = hi - lo + 1;
	}
	fclose(fp);
}

void machine_init(void)
{
	struct sysinfo si;

	machine.chipset = CHIPSET_AGA;
	read_hardware();
	if (machine.amiga)
		read_iomem();
	if (!machine.chip_size)
		machine.chip_size = 0x200000;
	if (!machine.eclock)
		machine.eclock = machine.pal ? 709379 : 715909;
	if (!machine.psfreq)
		machine.psfreq = machine.pal ? 50 : 60;
	if (!machine.nregions) {
		uint64_t t = 0;
		if (!sysinfo(&si))
			t = (uint64_t)si.totalram * si.mem_unit;
		/* capped, so programs that add the figures up in 32 bits
		   stay sane */
		machine.region[0].size = t > 0x40000000 ? 0x40000000 :
					 t ? t : 0x1000000;
		machine.nregions = 1;
	}
}

/* GfxBase->ChipRevBits0 */
uint8_t machine_chiprev(void)
{
	switch (machine.chipset) {
	case CHIPSET_AGA:
		return 0x0f;    /* GFXF_AA_ALICE|GFXF_AA_LISA|HR_AGNUS|HR_DENISE */
	case CHIPSET_ECS:
		return 0x01 | (machine.hr_denise ? 0x02 : 0);
	}
	return 0;
}

/* VPOSR's Agnus/Alice identification and DENISEID, as the chips answer */
uint16_t machine_vposr(void)
{
	static const uint16_t id[3][2] = {
		{ 0x1000, 0x0000 },     /* OCS: 8361 NTSC, 8367 PAL */
		{ 0x3000, 0x2000 },     /* ECS: 8372 */
		{ 0x3300, 0x2300 },     /* AGA: Alice */
	};
	return id[machine.chipset][machine.pal];
}

uint16_t machine_deniseid(void)
{
	if (machine.chipset == CHIPSET_AGA)
		return 0x00f8;          /* Lisa */
	if (machine.hr_denise)
		return 0x00fc;          /* 8373 */
	return 0xffff;                  /* the original Denise has none */
}

static struct MemHeader *region_header(uint32_t base, uint32_t size)
{
	char *p = mmap(PTR(base), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS |
		       MAP_NORESERVE, -1, 0);

	if (p == MAP_FAILED || mprotect(p, 4096, PROT_READ | PROT_WRITE)) {
		if (p != MAP_FAILED)
			munmap(p, size);
		return NULL;
	}
	memset(p, 0, 4096);
	return (struct MemHeader *)p;
}

static struct MemHeader *add_region(struct ExecBase *e, uint32_t base,
				    uint32_t size, uint16_t attr, int pri,
				    const char *name)
{
	struct MemHeader *mh;
	char *nm;

	size &= ~0xfffu;
	while (!(mh = region_header(base, size)) && size > 0x100000)
		size /= 2;
	if (!mh)
		return NULL;
	nm = (char *)(mh + 1);
	strcpy(nm, name);
	mh->mh_Node.ln_Type = NT_MEMORY;
	mh->mh_Node.ln_Pri = pri;
	mh->mh_Node.ln_Name = APTR_OF(nm);
	mh->mh_Attributes = attr;
	mh->mh_Lower = APTR_OF(mh) + 64;
	mh->mh_Upper = APTR_OF(mh) + size;
	mh->mh_Free = 0;
	exec_enqueue(&e->MemList, &mh->mh_Node);
	return mh;
}

static struct MemHeader *chip_mh, *fast_mh[MACHINE_REGIONS];

void machine_memlist(struct ExecBase *e)
{
	int i;

	for (i = 0; i < machine.nregions; i++)
		fast_mh[i] = add_region(e, machine.region[i].base,
					machine.region[i].size,
					MEMF_PUBLIC | MEMF_FAST, 0,
					"expansion memory");
	chip_mh = add_region(e, 0, machine.chip_size,
			     MEMF_PUBLIC | MEMF_CHIP | MEMF_LOCAL | MEMF_24BITDMA,
			     -10, "chip memory");
	machine_memfree(NULL, NULL, NULL, NULL);
}

/*
 * The figures AvailMem reports, kept in the headers' mh_Free as well:
 * Linux's free memory as fast RAM, shared out over the regions, and
 * three quarters of the token chip RAM.
 */
void machine_memfree(uint32_t *chip_total, uint32_t *chip_free,
		     uint32_t *fast_total, uint32_t *fast_free)
{
	uint64_t total = 0, avail = amem_avail(), left;
	int i;

	for (i = 0; i < machine.nregions; i++)
		if (fast_mh[i])
			total += fast_mh[i]->mh_Upper - APTR_OF(fast_mh[i]);
	if (avail > total)
		avail = total;
	left = avail;
	for (i = 0; i < machine.nregions; i++) {
		uint64_t sz, share;
		if (!fast_mh[i])
			continue;
		sz = fast_mh[i]->mh_Upper - APTR_OF(fast_mh[i]);
		share = total ? avail * sz / total : 0;
		if (share > left)
			share = left;
		fast_mh[i]->mh_Free = share;
		left -= share;
	}
	if (chip_mh)
		chip_mh->mh_Free = (chip_mh->mh_Upper - APTR_OF(chip_mh)) / 4 * 3;
	if (chip_total)
		*chip_total = chip_mh ? chip_mh->mh_Upper - APTR_OF(chip_mh) : 0;
	if (chip_free)
		*chip_free = chip_mh ? chip_mh->mh_Free : 0;
	if (fast_total)
		*fast_total = total;
	if (fast_free)
		*fast_free = avail;
}
