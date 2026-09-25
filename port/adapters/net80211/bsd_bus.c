/*
 * @file
 * @brief bus_dma(9)/bus_space(9)/pool_cache(9) backend for the USB
 * host line.
 *
 * The kernel runs identity-mapped over a 96 MiB RAM window, so a DMA
 * address is the virtual address of the buffer (the same model the
 * compat bus.h documents for the PCIe port). The bus_dmamem_* family
 * allocates from a dedicated static pool through a private TLSF
 * instance (O(1) alloc/free/memalign, byte-identical allocator to the
 * system heap), bus_dmamap_sync carries the CPU<->device ownership
 * transition with unconditional dcache maintenance - the RK3568 USB
 * masters sit behind a non-coherent port, and a missed sync fails
 * silently (AGENTS.md discipline).
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "tlsf.h"

#include <sys/types.h>
#include <sys/bus.h>
#include <sys/pool.h>
#include <sys/errno.h>
#include <sys/kmem.h>

#include "cmsis_os2.h"

/* cache maintenance: port/aarch64/cache.c */
extern void board_dcache_flush(uintptr_t addr, unsigned long size);
extern void board_dcache_invalidate(uintptr_t addr, unsigned long size);

/* memory barrier before the device touches or after it filled memory */
void
__wlan_barrier(void)
{
	__asm volatile ("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------
 * the DMA pool: static, identity-mapped, 4096-aligned
 */

#define WLAN_DMA_POOL_BYTES (512 * 1024)

static uint8_t wlan_dma_pool[WLAN_DMA_POOL_BYTES]
    __attribute__((aligned(4096)));
static tlsf_t wlan_dma_tlsf;

static void
wlan_dma_init(void)
{
	if (wlan_dma_tlsf != NULL) {
		return;
	}
	wlan_dma_tlsf = tlsf_create_with_pool(wlan_dma_pool,
	    WLAN_DMA_POOL_BYTES);
}

/* the whole RAM window is DMA-reachable: one shared tag instance */
struct bus_dma_tag wlan_bus_dma_tag_store;
bus_dma_tag_t wlan_bus_dma_tag = &wlan_bus_dma_tag_store;

/* ------------------------------------------------------------------
 * bus_space barrier
 */

void
wlan_bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h,
	bus_size_t o, bus_size_t len, int flags)
{
	(void) t; (void) h; (void) o; (void) len; (void) flags;
	__wlan_barrier();
}

/* ------------------------------------------------------------------
 * bus_dma(9): identity mapping, single-segment
 */

int
bus_dmamem_alloc(bus_dma_tag_t tag, bus_size_t size,
	bus_size_t alignment, bus_size_t boundary,
	struct bus_dma_segment *segs, int nsegs, int *rsegs, int flags)
{
	void *p;

	(void) tag; (void) boundary; (void) flags;

	if (nsegs < 1) {
		return ENOMEM;
	}
	wlan_dma_init();
	if (alignment < 8) {
		alignment = 8;
	}
	p = tlsf_memalign(wlan_dma_tlsf, alignment, size);
	if (p == NULL) {
		return ENOMEM;
	}
	segs[0].ds_addr = (bus_addr_t) p;
	segs[0].ds_len = size;
	*rsegs = 1;
	return 0;
}

void
bus_dmamem_free(bus_dma_tag_t tag, struct bus_dma_segment *segs, int nsegs)
{
	(void) tag;

	if (nsegs >= 1 && wlan_dma_tlsf != NULL) {
		tlsf_free(wlan_dma_tlsf, (void *) segs[0].ds_addr);
	}
}

int
bus_dmamem_map(bus_dma_tag_t tag, struct bus_dma_segment *segs, int nsegs,
	bus_size_t size, void **kvap, int flags)
{
	(void) tag; (void) size; (void) flags;

	if (nsegs < 1) {
		return ENOMEM;
	}
	/* identity map: the DMA address is the kernel address */
	*kvap = (void *) segs[0].ds_addr;
	return 0;
}

void
bus_dmamem_unmap(bus_dma_tag_t tag, void *kva, bus_size_t size)
{
	(void) tag; (void) kva; (void) size;
}

int
bus_dmamap_create(bus_dma_tag_t tag, bus_size_t size, int nsegments,
	bus_size_t maxsegsz, bus_size_t boundary, int flags,
	bus_dmamap_t *dmamp)
{
	bus_dmamap_t map;

	(void) tag; (void) size; (void) nsegments; (void) boundary;
	(void) flags;

	map = kmem_zalloc(sizeof(*map), KM_SLEEP);
	if (map == NULL) {
		return ENOMEM;
	}
	map->dm_maxsegsz = maxsegsz;
	*dmamp = map;
	return 0;
}

void
bus_dmamap_destroy(bus_dma_tag_t tag, bus_dmamap_t map)
{
	(void) tag;
	kmem_free(map, sizeof(*map));
}

int
bus_dmamap_load(bus_dma_tag_t tag, bus_dmamap_t map, void *va,
	bus_size_t size, void *ctx, int flags)
{
	(void) tag; (void) ctx; (void) flags;

	map->dm_segs[0].ds_addr = (bus_addr_t) va;
	map->dm_segs[0].ds_len = size;
	map->dm_nsegs = 1;
	map->dm_mapsize = size;
	return 0;
}

int
bus_dmamap_load_mbuf(bus_dma_tag_t tag, bus_dmamap_t map, struct mbuf *m0,
	int flags)
{
	(void) tag; (void) flags;

	/* the compat mbuf model is one contiguous cluster per mbuf */
	map->dm_segs[0].ds_addr = (bus_addr_t) m0;
	map->dm_nsegs = 1;
	map->dm_mapsize = 0;
	return 0;
}

void
bus_dmamap_unload(bus_dma_tag_t tag, bus_dmamap_t map)
{
	(void) tag;

	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
}

void
bus_dmamap_sync(bus_dma_tag_t tag, bus_dmamap_t map, bus_size_t offset,
	bus_size_t len, int ops)
{
	uintptr_t addr;

	(void) tag;

	if (map == NULL || map->dm_nsegs < 1 || len == 0) {
		return;
	}
	addr = (uintptr_t) map->dm_segs[0].ds_addr + offset;

	if (ops & BUS_DMASYNC_PREREAD) {
		/* discard dirty lines before the device fills the buffer */
		board_dcache_invalidate(addr, len);
	}
	if (ops & BUS_DMASYNC_PREWRITE) {
		/* push CPU writes out before the device reads them */
		board_dcache_flush(addr, len);
	}
	if (ops & BUS_DMASYNC_POSTREAD) {
		board_dcache_invalidate(addr, len);
	}
	/* BUS_DMASYNC_POSTWRITE: nothing to do */
	__wlan_barrier();
}

int
bus_dmatag_subregion(bus_dma_tag_t tag, bus_addr_t low, bus_size_t high,
	bus_dma_tag_t *newtagp, int flags)
{
	(void) low; (void) high; (void) flags;

	/* the whole RAM window is DMA-reachable: hand back the same tag */
	*newtagp = tag;
	return 0;
}

void
bus_dmatag_destroy(bus_dma_tag_t tag)
{
	(void) tag;
}

int boothowto; /* boot flags: none (sys/reboot.h vocabulary) */

/* ------------------------------------------------------------------
 * pool_cache(9): no free lists - allocate/release straight through
 */

pool_cache_t
pool_cache_init(size_t size, size_t align, size_t align_off,
	size_t unused, const char *wchan, struct pool_allocator *pa, int ipl,
	int (*ctor)(void *, void *, int), void (*dtor)(void *, void *),
	void *arg)
{
	struct pool_cache *pc;

	(void) align_off; (void) unused; (void) wchan; (void) pa; (void) ipl;
	(void) ctor; (void) dtor; (void) arg;

	pc = kmem_zalloc(sizeof(*pc), KM_SLEEP);
	if (pc == NULL) {
		return NULL;
	}
	pc->pc_size = size;
	pc->pc_align = (align != 0) ? align : 8;
	return pc;
}

void
pool_cache_destroy(pool_cache_t pc)
{
	kmem_free(pc, sizeof(*pc));
}

void *
pool_cache_get(pool_cache_t pc, int flags)
{
	void *p;

	wlan_dma_init();
	if (flags & PR_WAITOK) {
		/* bounded retry: the callers are attach/transfer paths */
		for (;;) {
			p = tlsf_memalign(wlan_dma_tlsf, pc->pc_align,
			    pc->pc_size);
			if (p != NULL) {
				break;
			}
			osDelay(1);
		}
	} else {
		p = tlsf_memalign(wlan_dma_tlsf, pc->pc_align, pc->pc_size);
	}
	if (p != NULL) {
		memset(p, 0, pc->pc_size);
	}
	return p;
}

void
pool_cache_put(pool_cache_t pc, void *obj)
{
	tlsf_free(wlan_dma_tlsf, obj);
}
