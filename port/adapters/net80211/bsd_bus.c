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

/* The USB line fits in half a megabyte; iwm does not - its fw load alone
 * walks ~256 KiB DMA segments and the five TX queues carry their TFD ring,
 * cmd buffers and scratch from the same pool (the abandoned fdt line died
 * on "could not allocate TX cmd DMA memory" at 512 KiB). 4 MiB keeps the
 * whole radio world comfortably inside the 96 MiB image RAM window. */
#define WLAN_DMA_POOL_BYTES (4 * 1024 * 1024)

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
 * bus_space(9): identity-map window-checked mapping
 */

/*
 * The windows this port grants.  Everything below 4 GiB sits in the
 * L1[3] Device window (mmu.c); the DesignWare DBI frames live above
 * 4 GiB in the dedicated L2 pages board_conf.h switches on.  A request
 * outside these windows is refused - the mapping would fault anyway,
 * and refusing names the mistake at the map call instead of an async
 * external abort deep inside an attach.
 */
static const struct wlan_bus_window {
	bus_addr_t w_base;
	bus_addr_t w_end;	/* exclusive */
} wlan_bus_windows[] = {
	{ 0xf0000000UL, 0xf0200000UL },	/* pcie ECAM config window */
	{ 0xf0200000UL, 0xf4000000UL },	/* pcie MEM window (30 MiB + gap) */
	{ 0xfd000000UL, 0xfe000000UL },	/* SoC low: GIC/CRU/GRF/PMUCRU */
	{ 0xfe000000UL, 0xff000000UL },	/* peripherals: pcie apb, pcie30phy */
	{ 0x3c0000000ULL, 0x3c0c00000ULL }, /* DWC DBI frames (pcie2x1/3x2) */
};

static int
wlan_bus_space_map_cb(void *cookie, bus_addr_t addr, bus_size_t size,
	int flags, bus_space_handle_t *hp)
{
	size_t n;

	(void) cookie; (void) flags;

	for (n = 0; n < sizeof(wlan_bus_windows) / sizeof(wlan_bus_windows[0]);
	    n++) {
		const struct wlan_bus_window *w = &wlan_bus_windows[n];

		if (addr >= w->w_base && addr + size <= w->w_end) {
			/* identity mapping: the handle is the address */
			*hp = (bus_space_handle_t) addr;
			return 0;
		}
	}
	printf("bus: bus_space_map OUT OF WINDOWS addr=%08llx size=%lx\n",
	    (unsigned long long) addr, (unsigned long) size);
	return ERANGE;
}

static struct bus_space wlan_bus_space_store = {
	.bs_cookie = NULL,
	.bs_map = wlan_bus_space_map_cb,
	.bs_unmap = NULL,
};

/* the singleton tag every bus backend hands out (compat bus.h) */
bus_space_tag_t wlan_bus_space_tag = &wlan_bus_space_store;

/* ------------------------------------------------------------------
 * bus_dma(9): identity mapping, single-segment
 */

/*
 * Guard rails.  A DMA master cannot sanity-check the address it is given:
 * on this SoC a write below the image line (ATF/OP-TEE territory) is
 * dropped while the controller still reports a clean transfer, which is
 * how a bad buffer address turns into "the device answered with garbage".
 * So every address handed to hardware is checked against the two facts
 * this port knows: the pool it allocates from, and the board's
 * DMA-reachable RAM window (port/board/rk3568/board_conf.h
 * BOARD_MMU_IMAGE_RAM_BASE; the 96M cap is the linker script's RAM_SIZE -
 * both repeated here because this unit compiles without the board include
 * path, exactly like the dcache externs above).
 */
#define WLAN_DMA_WINDOW_BASE	0x0a000000UL
#define WLAN_DMA_WINDOW_END	0x10000000UL

static const uintptr_t wlan_dma_pool_base = (uintptr_t) wlan_dma_pool;

/* forensics: the shape the system heap learned to carry (heap.c) - a
 * bounded op ring plus tlsf_check, so a corrupted pool names the
 * allocating/freeing sequence instead of handing out junk pointers */
#define DMA_OP_RING_N 24

struct dma_op {
	uintptr_t ra;			/* __builtin_return_address(0) */
	uintptr_t ptr;
	uint32_t size;
	uint8_t is_free;
};

static struct dma_op dma_ops[DMA_OP_RING_N];
static volatile unsigned dma_op_i;
static int dma_pool_broken;

static void
dma_pool_note(uintptr_t ra, uintptr_t ptr, uint32_t size, int is_free)
{
	struct dma_op *op = &dma_ops[dma_op_i % DMA_OP_RING_N];

	op->ra = ra;
	op->ptr = ptr;
	op->size = size;
	op->is_free = (uint8_t) is_free;
	dma_op_i++;
}

static void
dma_pool_check(const char *what, uintptr_t ra, uintptr_t ptr, uint32_t size)
{
	unsigned int i;

	if (dma_pool_broken || wlan_dma_tlsf == NULL) {
		return;
	}
	if (tlsf_check(wlan_dma_tlsf) == 0) {
		return;
	}
	dma_pool_broken = 1;
	printf("usb: dma pool TLSF CHECK FAILED after %s ptr=%08lx size=%lu "
	    "ra=%08lx\n", what, (unsigned long) ptr, (unsigned long) size,
	    (unsigned long) ra);
	for (i = 0; i < DMA_OP_RING_N; i++) {
		const struct dma_op *op =
		    &dma_ops[(dma_op_i + i) % DMA_OP_RING_N];

		if (op->ra == 0) {
			continue;
		}
		printf("usb: dma op %s ptr=%08lx size=%lu ra=%08lx\n",
		    op->is_free ? "free" : "alloc", (unsigned long) op->ptr,
		    (unsigned long) op->size, (unsigned long) op->ra);
	}
}

static int
dma_in_pool(uintptr_t p, size_t size)
{
	return p >= wlan_dma_pool_base &&
	    p + size <= wlan_dma_pool_base + WLAN_DMA_POOL_BYTES;
}

static int
dma_in_window(uintptr_t p, size_t size)
{
	return p >= WLAN_DMA_WINDOW_BASE && p + size <= WLAN_DMA_WINDOW_END;
}

/* the range, for the platform dump */
void
wlan_dma_pool_range(uintptr_t *base, size_t *len, uintptr_t *win_end)
{
	*base = wlan_dma_pool_base;
	*len = WLAN_DMA_POOL_BYTES;
	*win_end = WLAN_DMA_WINDOW_END;
}

int
bus_dmamem_alloc(bus_dma_tag_t tag, bus_size_t size,
	bus_size_t alignment, bus_size_t boundary,
	struct bus_dma_segment *segs, int nsegs, int *rsegs, int flags)
{
	void *p;
	uintptr_t ra = (uintptr_t) __builtin_return_address(0);

	(void) tag; (void) boundary; (void) flags;

	if (nsegs < 1) {
		return ENOMEM;
	}
	wlan_dma_init();
	if (alignment < 8) {
		alignment = 8;
	}
	p = tlsf_memalign(wlan_dma_tlsf, alignment, size);
	dma_pool_note(ra, (uintptr_t) p, (uint32_t) size, 0);
	if (p == NULL) {
		return ENOMEM;
	}
	if (!dma_in_pool((uintptr_t) p, size)) {
		/* refuse: programming this into a qTD is worse than failing */
		printf("usb: bus_dmamem_alloc OUT OF POOL ptr=%08lx size=%lu "
		    "align=%lu ra=%08lx pool=%08lx..%08lx\n",
		    (unsigned long) (uintptr_t) p, (unsigned long) size,
		    (unsigned long) alignment, (unsigned long) ra,
		    (unsigned long) wlan_dma_pool_base,
		    (unsigned long) (wlan_dma_pool_base + WLAN_DMA_POOL_BYTES));
		dma_pool_check("out-of-pool alloc", ra, (uintptr_t) p,
		    (uint32_t) size);
		return ENOMEM;
	}
	dma_pool_check("alloc", ra, (uintptr_t) p, (uint32_t) size);
	segs[0].ds_addr = (bus_addr_t) p;
	segs[0].ds_len = size;
	*rsegs = 1;
	return 0;
}

void
bus_dmamem_free(bus_dma_tag_t tag, struct bus_dma_segment *segs, int nsegs)
{
	uintptr_t ra = (uintptr_t) __builtin_return_address(0);

	(void) tag;

	if (nsegs >= 1 && wlan_dma_tlsf != NULL) {
		dma_pool_note(ra, (uintptr_t) segs[0].ds_addr,
		    (uint32_t) segs[0].ds_len, 1);
		tlsf_free(wlan_dma_tlsf, (void *) segs[0].ds_addr);
		dma_pool_check("free", ra, (uintptr_t) segs[0].ds_addr,
		    (uint32_t) segs[0].ds_len);
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

	if (!dma_in_window((uintptr_t) va, size)) {
		printf("usb: bus_dmamap_load OUT OF WINDOW va=%08lx size=%lu "
		    "ra=%08lx (window %08lx..%08lx)\n", (unsigned long) (uintptr_t) va,
		    (unsigned long) size,
		    (unsigned long) (uintptr_t) __builtin_return_address(0),
		    (unsigned long) WLAN_DMA_WINDOW_BASE,
		    (unsigned long) WLAN_DMA_WINDOW_END);
		return EINVAL;
	}
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

	/* The DMA address must be the address the stack reads and writes -
	 * mtod(m) - i.e. the mbuf's DATA area, not the header.  The compat
	 * allocation puts the cluster behind the header (m_data = cluster or
	 * cluster + MH_ALIGN, 256-aligned); a descriptor programmed with the
	 * header address lands the device's frame ahead of where the driver
	 * looks, silently: for iwm the RX ring then parsed garbage and the
	 * uCode ALIVE response was never seen (the interrupt arrived, the
	 * driver's uc_intr was never set, the firmware load timed out).
	 * m_data is also the 256-byte alignment the Intel RX descriptors
	 * require.  The USB line is unaffected - its transfers move data
	 * through usb_mem blocks and never hand an mbuf address to a master.
	 */
	if (m0 != NULL) {
		map->dm_segs[0].ds_addr = (bus_addr_t)(uintptr_t)
		    (m0->m_data != NULL ? m0->m_data : (void *) m0);
		map->dm_segs[0].ds_len = (bus_size_t) m0->m_len;
		map->dm_mapsize = (bus_size_t) m0->m_len;
	} else {
		map->dm_segs[0].ds_addr = 0;
		map->dm_segs[0].ds_len = 0;
		map->dm_mapsize = 0;
	}
	map->dm_nsegs = 1;
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
