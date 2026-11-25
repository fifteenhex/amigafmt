/*
 * Memory the Amiga program sees.
 *
 * Code is loaded here and programs build code in AllocMem()ed memory
 * too, and qemu-m68k refuses to execute anything not mapped PROT_EXEC,
 * which glibc's heap is not.  So this is a small boundary-tag allocator
 * over RWX mmap arenas.  Blocks are 16-byte aligned (AmigaOS promises 8).
 */
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "af.h"

#define ARENA   (4u << 20)
#define BIG     (1u << 20)
#define USED    1u
#define MAPPED  2u

struct blk {
	uint32_t size;          /* whole block incl. header, | flags */
	uint32_t prev;          /* size of the block before, 0 = arena start */
	uint32_t pad[2];
	struct blk *fnext, *fprev;      /* free list (free blocks only) */
};

#define HDR 16
#define SZ(b) ((b)->size & ~15u)
#define NEXT(b) ((struct blk *)((char *)(b) + SZ(b)))

static struct blk freelist = { 0, 0, {0, 0}, &freelist, &freelist };

static void fl_add(struct blk *b)
{
	b->fnext = freelist.fnext;
	b->fprev = &freelist;
	freelist.fnext->fprev = b;
	freelist.fnext = b;
}

static void fl_del(struct blk *b)
{
	b->fprev->fnext = b->fnext;
	b->fnext->fprev = b->fprev;
}

static void *map_rwx(uint32_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

static int grow(void)
{
	char *a = map_rwx(ARENA);
	struct blk *b, *end;

	if (!a)
		return 0;
	b = (struct blk *)a;
	b->size = ARENA - HDR;
	b->prev = 0;
	end = NEXT(b);                  /* a used, zero-size sentinel */
	end->size = 0 | USED;
	end->prev = SZ(b);
	fl_add(b);
	return 1;
}

void *amem_alloc(uint32_t size)
{
	uint32_t need = (size + HDR + 15) & ~15u;
	struct blk *b;

	if (size > 0x7fffffff)
		return NULL;
	if (need >= BIG) {
		uint32_t len = (need + 4095) & ~4095u;
		b = map_rwx(len);
		if (!b)
			return NULL;
		b->size = len | MAPPED | USED;
		return (char *)b + HDR;
	}
	for (;;) {
		for (b = freelist.fnext; b != &freelist; b = b->fnext)
			if (SZ(b) >= need)
				goto found;
		if (!grow())
			return NULL;
	}
found:
	fl_del(b);
	if (SZ(b) - need >= 64) {
		struct blk *rest = (struct blk *)((char *)b + need);
		rest->size = SZ(b) - need;
		rest->prev = need;
		NEXT(rest)->prev = SZ(rest);
		b->size = need;
		fl_add(rest);
	}
	b->size |= USED;
	memset((char *)b + HDR, 0, SZ(b) - HDR);
	return (char *)b + HDR;
}

void amem_free(void *p)
{
	struct blk *b, *n;

	if (!p)
		return;
	b = (struct blk *)((char *)p - HDR);
	if (!(b->size & USED)) {
		af_warn("freeing memory twice at %p", p);
		return;
	}
	if (b->size & MAPPED) {
		munmap(b, SZ(b));
		return;
	}
	b->size &= ~USED;
	n = NEXT(b);
	if (!(n->size & USED)) {                /* merge forward */
		fl_del(n);
		b->size += SZ(n);
		NEXT(b)->prev = SZ(b);
	}
	if (b->prev) {                          /* merge backward */
		struct blk *pb = (struct blk *)((char *)b - b->prev);
		if (!(pb->size & USED)) {
			fl_del(pb);
			pb->size += SZ(b);
			NEXT(pb)->prev = SZ(pb);
			b = pb;
		}
	}
	fl_add(b);
}

uint32_t amem_avail(void)
{
	long pages = sysconf(_SC_AVPHYS_PAGES);
	long psz = sysconf(_SC_PAGESIZE);
	uint64_t v = (uint64_t)pages * (uint64_t)psz;

	return v > 0x7fffffff ? 0x7fffffff : (uint32_t)v;
}

/*
 * The 68040/060 have split caches and copyback data caches: code written
 * through the data cache must be pushed and the instruction cache
 * invalidated before it is executed.  sys_cacheflush, FLUSH_SCOPE_ALL,
 * FLUSH_CACHE_BOTH.
 */
void cache_flush(void *p, uint32_t len)
{
	syscall(SYS_cacheflush, (unsigned long)p, 3, 3, (unsigned long)len);
}
