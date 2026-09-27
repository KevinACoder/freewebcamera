/*
 * @file
 * @brief pool_cache(9) shell over the port allocator.
 *
 * Only what ehci.c's xfer/ring pools need: fixed-size objects, no
 * destructor, no interrupt pools. Backed by the same allocator as the
 * bus_dma backend (bsd_bus.c).
 */

#ifndef _COMPAT_SYS_POOL_H_
#define _COMPAT_SYS_POOL_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/device.h>
#include <sys/pmf.h>

struct pool_cache {
	size_t pc_size;
	size_t pc_align;
};
typedef struct pool_cache *pool_cache_t;
struct pool_allocator;

#define PR_NOWAIT	0x02
#define PR_WAITOK	0x01
#define PR_LIMITFAIL	0x04
#define PR_ZERO		0x08

pool_cache_t pool_cache_init(size_t size, size_t align, size_t align_off,
	size_t unused, const char *wchan, struct pool_allocator *pa, int ipl,
	int (*ctor)(void *, void *, int), void (*dtor)(void *, void *),
	void *arg);
void pool_cache_destroy(pool_cache_t pc);
void *pool_cache_get(pool_cache_t pc, int flags);
void pool_cache_put(pool_cache_t pc, void *obj);

/* the scatter_buf mmap path asks the VM for physical pages; this port
 * is identity-mapped (virtual == physical, see bsd_bus.c), so the pmap
 * face is a shell */
typedef struct pmap *pmap_t;
pmap_t pmap_kernel(void);
int pmap_extract(pmap_t, vaddr_t, paddr_t *);

#endif /* _COMPAT_SYS_POOL_H_ */
