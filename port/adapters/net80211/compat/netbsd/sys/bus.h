/*
 * @file
 * @brief bus_space and bus_dma for the PCIe port.
 *
 * MMIO runs as volatile accesses on the identity-mapped register
 * window. The platform has no SMMU in the PCIe path and the kernel
 * runs identity mapped, so a DMA address is the virtual address of
 * the buffer; bus_dmamap_sync carries the ownership transition with
 * dcache flush/invalidate and a dsb, the same discipline the in-tree
 * NVMe driver follows on this board.
 *
 * @date 10.09.2026
 * @author zhugengyu
 */

#ifndef _COMPAT_SYS_BUS_H_
#define _COMPAT_SYS_BUS_H_

#include <sys/cdefs.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------
 * bus_space(9): little-endian device, little-endian host.
 */

typedef uintptr_t bus_space_handle_t;
typedef uintptr_t bus_addr_t;
typedef uintptr_t bus_size_t;

/* The tag is a pointer to a method struct (the NetBSD shape, reduced to
 * what the imported code reaches): pcihost_fdt copies a tag's struct
 * and replaces bs_map to translate PCI bus addresses through the ranges
 * window, so bs_map must be real indirection.  The register accessors
 * stay direct volatile loads on the identity-mapped handle - the USB
 * world has always discarded the tag.
 */

struct bus_space {
	void *bs_cookie;
	int	(*bs_map)(void *, bus_addr_t, bus_size_t, int,
		    bus_space_handle_t *);
	void	(*bs_unmap)(void *, bus_space_handle_t, bus_size_t);
};

typedef struct bus_space *bus_space_tag_t;

/* the singleton tag every bus backend hands out (bsd_bus.c) */
extern bus_space_tag_t wlan_bus_space_tag;

/* pci.c probes this around its dmat fields */
#define	BUS_DMA_TAG_VALID(t)	((t) != NULL)

static inline uint8_t bus_space_read_1(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o) {
	(void) t;
	return *(volatile uint8_t *) (h + o);
}

static inline uint16_t bus_space_read_2(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o) {
	(void) t;
	return *(volatile uint16_t *) (h + o);
}

static inline uint32_t bus_space_read_4(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o) {
	(void) t;
	return *(volatile uint32_t *) (h + o);
}

static inline void bus_space_write_1(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o, uint8_t v) {
	(void) t;
	*(volatile uint8_t *) (h + o) = v;
}

static inline void bus_space_write_2(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o, uint16_t v) {
	(void) t;
	*(volatile uint16_t *) (h + o) = v;
}

static inline void bus_space_write_4(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o, uint32_t v) {
	(void) t;
	*(volatile uint32_t *) (h + o) = v;
}

/* 64-bit register windows exist (xhci.c's XHCI_USE_BUS_SPACE_8 path);
 * every register the driver touches this way is 8-aligned and the
 * identity Device mapping carries a single 8-byte store as one AXI
 * beat - what the xHCI spec means by a Qword register write (CRCR
 * ignores the two-Dword form outright). */
static inline uint64_t bus_space_read_8(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o) {
	(void) t;
	return *(volatile uint64_t *) (h + o);
}

static inline void bus_space_write_8(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o, uint64_t v) {
	(void) t;
	*(volatile uint64_t *) (h + o) = v;
}

/* the identity map makes a subregion a plain offset; xhci_init slices
 * its capability/operational/doorbell/runtime windows this way */
static inline int bus_space_subregion(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o, bus_size_t s,
	bus_space_handle_t *hp) {
	(void) t; (void) s;
	*hp = h + o;
	return 0;
}

#define BUS_SPACE_BARRIER_READ  0x01
#define BUS_SPACE_BARRIER_WRITE 0x02

/* bus_space_map flags (the subset pcihost_fdt/rk_pcie use) */
#define	BUS_SPACE_MAP_CACHEABLE		0x01
#define	BUS_SPACE_MAP_LINEAR		0x02
#define	BUS_SPACE_MAP_PREFETCHABLE	0x04
#define	BUS_SPACE_MAP_NONPOSTED		0x08

/* identity-map window-checked mapping, implemented by the bus backend
 * (bsd_bus.c): the kernel runs identity-mapped, so a handle is the
 * physical address, but only windows the MMU actually covers are
 * granted - an unmapped window faults loudly by design (mmu.c).
 * Dispatch goes through the tag so pcihost_fdt's per-bus translation
 * wrappers compose. */
static inline int bus_space_map(bus_space_tag_t t, bus_addr_t addr,
	bus_size_t size, int flags, bus_space_handle_t *hp) {
	return t->bs_map(t->bs_cookie, addr, size, flags, hp);
}

static inline void bus_space_unmap(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t size) {
	if (t->bs_unmap != NULL) {
		t->bs_unmap(t->bs_cookie, h, size);
	}
}

/* dsb-backed barrier; implemented by the PCIe backend */
void wlan_bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h,
	bus_size_t o, bus_size_t len, int flags);

static inline void bus_space_barrier(bus_space_tag_t t,
	bus_space_handle_t h, bus_size_t o, bus_size_t len, int flags) {
	wlan_bus_space_barrier(t, h, o, len, flags);
}

/* ------------------------------------------------------------------
 * bus_dma(9)
 */

#define BUS_DMA_WAITOK   0x00
#define BUS_DMA_NOWAIT   0x01
#define BUS_DMA_ALLOCNOW 0x02
#define BUS_DMA_COHERENT 0x04
#define BUS_DMA_BUS1     0x10
#define BUS_DMA_READ     0x20
#define BUS_DMA_WRITE    0x40

#define BUS_DMASYNC_PREREAD   0x01
#define BUS_DMASYNC_POSTREAD  0x02
#define BUS_DMASYNC_PREWRITE  0x04
#define BUS_DMASYNC_POSTWRITE 0x08

/* pointer-tagged like the NetBSD arm port so callers can compare tags */
struct bus_dma_tag {
	int unused;
};

typedef struct bus_dma_tag *bus_dma_tag_t;

#define BUS_ADDR_HI32(a) ((uint32_t) ((uint64_t) (a) >> 32))
#define BUS_ADDR_LO32(a) ((uint32_t) (a))

struct bus_dma_segment {
	bus_addr_t ds_addr;
	bus_size_t ds_len;
};

typedef struct bus_dma_segment bus_dma_segment_t;

typedef struct bus_dmamap {
	struct bus_dma_segment dm_segs[1];
	int dm_nsegs;
	bus_size_t dm_mapsize;
	bus_size_t dm_maxsegsz;
} *bus_dmamap_t;

/* every caller hands the same global tag; there is nothing to
 * configure on this platform. The backends define it. */
extern bus_dma_tag_t wlan_bus_dma_tag;

int bus_dmamap_create(bus_dma_tag_t tag, bus_size_t size, int nsegments,
	bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t *dmamp);
void bus_dmamap_destroy(bus_dma_tag_t tag, bus_dmamap_t map);
int bus_dmamem_alloc(bus_dma_tag_t tag, bus_size_t size,
	bus_size_t alignment, bus_size_t boundary,
	struct bus_dma_segment *segs, int nsegs, int *rsegs, int flags);
void bus_dmamem_free(bus_dma_tag_t tag, struct bus_dma_segment *segs,
	int nsegs);
int bus_dmamem_map(bus_dma_tag_t tag, struct bus_dma_segment *segs,
	int nsegs, bus_size_t size, void **kvap, int flags);
void bus_dmamem_unmap(bus_dma_tag_t tag, void *kva, bus_size_t size);
int bus_dmamap_load(bus_dma_tag_t tag, bus_dmamap_t map, void *va,
	bus_size_t size, void *ctx, int flags);
struct mbuf;
int bus_dmamap_load_mbuf(bus_dma_tag_t tag, bus_dmamap_t map,
	struct mbuf *m, int flags);
void bus_dmamap_unload(bus_dma_tag_t tag, bus_dmamap_t map);
/* Carries the CPU<->device ownership transition through the dcache;
 * implemented by the PCIe backend. */
void bus_dmamap_sync(bus_dma_tag_t tag, bus_dmamap_t map, bus_size_t offset,
	bus_size_t len, int ops);

/* tag subregion: the whole RAM window is DMA-reachable, so this hands
 * the same tag back (ehci.c probes with it once) */
int bus_dmatag_subregion(bus_dma_tag_t tag, bus_addr_t low, bus_size_t high,
	bus_dma_tag_t *newtagp, int flags);
void bus_dmatag_destroy(bus_dma_tag_t tag);

#endif /* _COMPAT_SYS_BUS_H_ */
