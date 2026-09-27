/*
 * @file   heap.c
 * @brief  Allocation surface for the ThreadX line, over the vendored TLSF.
 *
 * The FreeRTOS image's allocation surface comes from the kernel's heap_4
 * running over ucHeap (port/adapters/freertos/heap.c). ThreadX has no
 * integrator heap at all, so this file is the ThreadX twin: the vendored
 * TLSF allocator (third-party/tlsf, O(1) malloc/free/memalign, BSD-2)
 * managing one static region, exported under the pvPortMalloc/vPortFree
 * names the in-tree consumers already call (thread stacks in
 * cmsis_os2_impl.c, the libbsd adapter, the sdmmc OSA layer).
 *
 * TLSF is not thread safe (its README says so); every entry point takes
 * _tx_thread_smp_protect/_unprotect - the primitive the kernel's own
 * TX_DISABLE expands to on this port. That makes the heap correct before
 * the kernel starts - the deferred thread-creation window allocates stacks
 * here, with IRQs already enabled and no scheduler yet - and nestable from
 * any core.
 *
 * The region lives in .bss, which mmu.c maps as Normal cacheable memory.
 *
 * M7 forensics: during the usb-wifi attach storm two blocked threads' saved
 * contexts were zeroed in place - the signature of heap aliasing. Every op
 * is recorded (with the caller's return address) into a small ring, and
 * tlsf_check() runs after each op; the first failure dumps the ring, so a
 * corrupted heap names the allocating/freeing line on the next boot.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tlsf.h"

/* The arena's base and size are the build configuration's keys
 * (CONFIG_HEAP_BASE / CONFIG_HEAP_BYTES): it is the first thing to run out
 * when a line lands and the first thing to break a boot when it is placed
 * over a fixed region, so it is a knob rather than a constant. */
#include "config.h"

extern void board_early_print(const char *s);

/* The port assembly's protection primitives (tx_thread_smp_protect.S /
 * tx_thread_smp_unprotect.S), spelled out to keep this file free of kernel
 * headers; UINT on this port is unsigned int. */
extern unsigned int _tx_thread_smp_protect(void);
extern void _tx_thread_smp_unprotect(unsigned int save);

/* 100 MB by default, one shot: the working lines no longer fit in 4 MB and the
 * failures are silent-until-fatal (iwm's RX ring ~1.1 MB, then the
 * supplicant's association took the last ~100 KB and the UVC dump path
 * had nothing left: "heap: alloc FAIL len=712 maxfree=152").
 *
 * The arena is NOT a .bss array: the port keeps fixed-address static
 * regions (the USB DMA pool at 0x0a793000, the wlan mbuf pools, the iwm
 * RX ring), and a 100 MB array inside .bss swallowed them - the first
 * such boot came up with ehci_irq=0, no camera and no wlan, because the
 * USB DMA structures sat inside the heap.  A fixed base far above the
 * image (512 MB) and every fixed region keeps .bss at its old size; the
 * MMU maps [0x0a000000, 0xc0000000) Normal cacheable (mmu.c: L1[0]'s
 * 2 MiB blocks plus two 1 GiB Normal blocks), so the whole arena is
 * ordinary cacheable RAM.  heap_ram_probe() proves it at boot. */
#define HEAP_BASE	((uint64_t) CONFIG_HEAP_BASE)
#define HEAP_BYTES	((size_t) CONFIG_HEAP_BYTES)

static uint8_t *const heap_region = (uint8_t *) (uintptr_t) HEAP_BASE;
static tlsf_t heap_tlsf;

/* One-shot RAM probe, run on the first allocation (i.e. during boot):
 * write/read a pattern across the arena so a mapping or DDR hole shows up
 * as a line in the boot log, not as a mysterious later fault.
 *
 * The offsets are derived from the configured size rather than fixed at
 * 0/32/64/96 MB: the arena is a key now, and a small one (the trim-proof
 * build) would otherwise be probed past its end. */
static void
heap_ram_probe(void)
{
	size_t offs[5];
	char msg[160];
	size_t n = 0;
	unsigned i;

	offs[n++] = 0u;
	offs[n++] = HEAP_BYTES / 4u;
	offs[n++] = HEAP_BYTES / 2u;
	offs[n++] = (HEAP_BYTES * 3u) / 4u;
	offs[n++] = HEAP_BYTES - sizeof(uint32_t);

	for (i = 0; i < n; i++) {
		volatile uint32_t *p =
		    (volatile uint32_t *) (void *) (heap_region + offs[i]);
		uint32_t v = 0xa5a50000u | i;

		*p = v;
		if (*p != v) {
			(void)snprintf(msg, sizeof(msg),
			    "heap: RAM probe FAIL at +%uKB (%p): wrote %08x "
			    "read %08x\n", (unsigned) (offs[i] >> 10),
			    (void *) p, v, (unsigned) *p);
			board_early_print(msg);
			return;
		}
	}
	(void)snprintf(msg, sizeof(msg),
	    "heap: RAM probe OK, arena %uKB at %p..%p\n",
	    (unsigned) (HEAP_BYTES >> 10), (void *) heap_region,
	    (void *) (heap_region + HEAP_BYTES));
	board_early_print(msg);
}

/* --- corruption forensics -------------------------------------------------- */

typedef struct heap_op {
	uintptr_t ra;			/* __builtin_return_address(0) */
	uintptr_t ptr;
	uint32_t  size;
	uint8_t   is_free;
} heap_op_t;

#define HEAP_RING_N	24U
static heap_op_t heap_ring[HEAP_RING_N];
static volatile unsigned heap_ring_i;
static int heap_broken;

static void heap_ring_push(uintptr_t ra, uintptr_t ptr, uint32_t size,
			   int is_free)
{
	heap_op_t *op = &heap_ring[heap_ring_i % HEAP_RING_N];

	op->ra = ra;
	op->ptr = ptr;
	op->size = size;
	op->is_free = (uint8_t) is_free;
	heap_ring_i++;
}

static void heap_check(uintptr_t ra)
{
	char msg[160];
	unsigned int i;

	if (heap_broken || heap_tlsf == (tlsf_t) 0) {
		return;
	}
	if (tlsf_check(heap_tlsf) == 0) {
		return;
	}
	heap_broken = 1;
	(void)snprintf(msg, sizeof(msg),
		       "heap: TLSF CHECK FAILED after ra=%08lx\n",
		       (unsigned long) ra);
	board_early_print(msg);
	for (i = 0U; i < HEAP_RING_N; i++) {
		const heap_op_t *op = &heap_ring[(heap_ring_i + i) %
						 HEAP_RING_N];

		if (op->ra == 0U) {
			continue;
		}
		(void)snprintf(msg, sizeof(msg),
			       "heap op %s %08lx sz=%lu ra=%08lx\n",
			       op->is_free ? "free" : "alloc",
			       (unsigned long) op->ptr,
			       (unsigned long) op->size,
			       (unsigned long) op->ra);
		board_early_print(msg);
	}
}

/* TLSF asserts on every internal invariant it can see (block headers,
 * free-list linkage). Freestanding build: assert() resolves to
 * __assert_func - make that loud instead of a link error. */
void __assert_func(const char *file, int line, const char *func,
		   const char *expr)
{
	char msg[160];

	(void)snprintf(msg, sizeof(msg),
		       "heap assert: %s:%u (%s) %s\n",
		       file, (unsigned) line, func != NULL ? func : "?", expr);
	board_early_print(msg);
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

/* --- the pvPort surface ----------------------------------------------------- */

/* Allocation accounting, reported by the `heap` shell command: the
 * pathological case this exists for is "allocations start failing while
 * the free count still looks healthy". */
unsigned wlan_heap_alloc_ok, wlan_heap_alloc_fail, wlan_heap_frees;
unsigned long wlan_heap_first_fail;
unsigned wlan_heap_double_free;
int wlan_heap_probe;

static void heap_maxfree_walker(void *ptr, size_t size, int used, void *user);

/* Double-free detection.  TLSF does not check: freeing a block that is
 * already free merges it with itself, which pulls blocks out of the free
 * lists while the block chain (and tlsf_check) still look consistent - the
 * observed signature is "allocation fails while a big enough free block is
 * reported".  The walk below is O(pool blocks), which is affordable here
 * (a few thousand blocks) and only for a diagnostic build: a pointer that
 * is already free is refused and counted instead of corrupting the pool. */
struct heap_dbl_check {
	uintptr_t target;
	int found;
	int used;
};

static void heap_dbl_walker(void *ptr, size_t size, int used, void *user)
{
	struct heap_dbl_check *d = user;

	if ((uintptr_t) ptr == d->target) {
		d->found = 1;
		d->used = used;
	}
}

static int heap_probe_used(uintptr_t ptr, int *used)
{
	struct heap_dbl_check d;

	d.target = ptr;
	d.found = 0;
	d.used = 0;
	tlsf_walk_pool(tlsf_get_pool(heap_tlsf), heap_dbl_walker, &d);
	if (!d.found) {
		return -1;
	}
	*used = d.used;
	return 0;
}

void *pvPortMalloc(size_t length)
{
	uint8_t *payload = NULL;
	unsigned int save;
	uintptr_t ra = (uintptr_t) __builtin_return_address(0);

	if (length == 0u || length > HEAP_BYTES) {
		wlan_heap_alloc_fail++;
		return NULL;
	}

	save = _tx_thread_smp_protect();
	if (heap_tlsf == (tlsf_t) 0) {
		heap_ram_probe();
		heap_tlsf = tlsf_create_with_pool(heap_region, HEAP_BYTES);
	}
	if (heap_tlsf != (tlsf_t) 0) {
		payload = tlsf_malloc(heap_tlsf, length);
	}
	if (payload != NULL) {
		heap_ring_push(ra, (uintptr_t) payload, (uint32_t) length, 0);
		wlan_heap_alloc_ok++;
		heap_check(ra);
	} else {
		wlan_heap_alloc_fail++;
		/* First failure: record the request size, the largest free
		 * block at that moment and TLSF's own verdict, once.  A
		 * failure with a big enough free block means the free lists
		 * (not the memory) are the problem. */
		if (wlan_heap_first_fail == 0) {
			static char fmsg[160];
			size_t maxfree = 0;

			wlan_heap_first_fail = length;
			wlan_heap_probe = 1;	/* arm the double-free probe */
			tlsf_walk_pool(tlsf_get_pool(heap_tlsf),
				       heap_maxfree_walker, &maxfree);
			(void)snprintf(fmsg, sizeof(fmsg),
				       "heap: alloc FAIL len=%lu maxfree=%lu "
				       "tlsf_check=%d\n",
				       (unsigned long) length,
				       (unsigned long) maxfree,
				       tlsf_check(heap_tlsf));
			board_early_print(fmsg);
		}
	}
	_tx_thread_smp_unprotect(save);
	return payload;
}

/* largest free block, used by the first-failure report above */
static void heap_maxfree_walker(void *ptr, size_t size, int used, void *user)
{
	size_t *maxfree = user;

	(void) ptr;
	if (!used && size > *maxfree) {
		*maxfree = size;
	}
}

void vPortFree(void *ptr)
{
	unsigned int save;
	uintptr_t ra = (uintptr_t) __builtin_return_address(0);

	if (ptr == NULL) {
		return;
	}
	/* TLSF stores its header immediately before the payload: a free of a
	 * pointer outside this region would still "work" - on memory that is
	 * not ours. Refuse and report instead (interior pointers inside the
	 * region remain the caller's bug; tlsf_check + the ring name them). */
	if ((uint8_t *) ptr < heap_region ||
	    (uint8_t *) ptr >= heap_region + HEAP_BYTES) {
		static char bad_msg[64];

		(void)snprintf(bad_msg, sizeof(bad_msg),
			       "heap: BAD FREE %08lx (refused)\n",
			       (unsigned long)(uintptr_t) ptr);
		board_early_print(bad_msg);
		return;
	}

	save = _tx_thread_smp_protect();
	/* The probe walks the whole pool, so it is armed only once an
	 * allocation has actually failed: normal operation pays nothing,
	 * and the first failure turns on the diagnosis and the refusal of
	 * a second free of the same block. */
	if (wlan_heap_probe) {
		int used = 0;

		if (heap_probe_used((uintptr_t) ptr, &used) == 0 && !used) {
			static char dmsg[64];

			wlan_heap_double_free++;
			(void)snprintf(dmsg, sizeof(dmsg),
				       "heap: DOUBLE FREE %08lx (refused)\n",
				       (unsigned long)(uintptr_t) ptr);
			board_early_print(dmsg);
			_tx_thread_smp_unprotect(save);
			return;
		}
	}
	heap_ring_push(ra, (uintptr_t) ptr,
		       (uint32_t) tlsf_block_size(ptr), 1);
	tlsf_free(heap_tlsf, ptr);
	wlan_heap_frees++;
	heap_check(ra);
	_tx_thread_smp_unprotect(save);
}

static void heap_free_walker(void *ptr, size_t size, int used, void *user)
{
	(void) ptr;

	if (!used) {
		*(size_t *) user += size;
	}
}

size_t xPortGetFreeHeapSize(void)
{
	unsigned int save;
	size_t free_bytes = 0;

	if (heap_tlsf == (tlsf_t) 0) {
		return HEAP_BYTES;
	}
	save = _tx_thread_smp_protect();
	/* v3.1 has no free-size query; walk the (single) pool. */
	tlsf_walk_pool(tlsf_get_pool(heap_tlsf), heap_free_walker,
		       &free_bytes);
	_tx_thread_smp_unprotect(save);
	return free_bytes;
}

/* --- census: what is outstanding right now ---------------------------------
 *
 * The 4 MB region carries the whole networking world, and the wireless
 * drivers allocate in bursts (an RX ring is 256 x 4.4 KB).  When an
 * allocation fails, the useful question is not the free count but *which*
 * blocks are live: this walk reports every block of 2 KB or more with its
 * size and its first four words, so a leaked mbuf is recognizable from the
 * shell (an mbuf's head reads m_flags/m_next/m_nextpkt, then m_data at
 * word 2 and m_len at word 3 - m_flags 3 is M_PKTHDR|M_EXT, i.e. a
 * header+cluster pair).
 */

struct heap_census {
	size_t total;			/* bytes in live blocks >= 2 KB */
	unsigned int blocks;
	unsigned int printed;
	size_t small_total;		/* everything live, for the balance */
	unsigned int small_blocks;
	/* The free side matters just as much: a request can fail while the
	 * free *count* looks healthy when the pool has no single block big
	 * enough, so the census reports the largest free block too. */
	size_t free_total;
	size_t free_max;
	unsigned int free_blocks;
};

static void heap_census_walker(void *ptr, size_t size, int used, void *user)
{
	struct heap_census *c = user;
	char msg[128];
	const uint32_t *w;
	char *out;
	int n;

	if (!used) {
		c->free_total += size;
		c->free_blocks++;
		if (size > c->free_max) {
			c->free_max = size;
		}
		return;
	}
	c->small_total += size;
	c->small_blocks++;
	if (size < 2048u) {
		return;
	}
	c->total += size;
	c->blocks++;
	if (c->printed >= 16u) {
		return;
	}
	c->printed++;

	w = (const uint32_t *) ptr;
	n = snprintf(msg, sizeof(msg),
		     "heap blk %08lx sz=%lu w=%08lx %08lx %08lx %08lx\n",
		     (unsigned long)(uintptr_t) ptr, (unsigned long) size,
		     (unsigned long) w[0], (unsigned long) w[1],
		     (unsigned long) w[2], (unsigned long) w[3]);
	if (n > 0 && (size_t) n < sizeof(msg)) {
		out = msg;
		board_early_print(out);
	} else {
		board_early_print("heap blk (unprintable)\n");
	}
}

void wlan_heap_census(void)
{
	struct heap_census c;
	char msg[256];
	unsigned int save;

	memset(&c, 0, sizeof(c));
	if (heap_tlsf == (tlsf_t) 0) {
		board_early_print("heap: not created yet\n");
		return;
	}
	save = _tx_thread_smp_protect();
	tlsf_walk_pool(tlsf_get_pool(heap_tlsf), heap_census_walker, &c);
	_tx_thread_smp_unprotect(save);

	(void)snprintf(msg, sizeof(msg),
		       "heap census: live=%lu B in %u block(s), >=2K: %lu B in %u; "
		       "free=%lu B in %u block(s) maxfree=%lu; "
		       "alloc ok=%u fail=%u free=%u dbl=%u firstfail=%lu\n",
		       (unsigned long) c.small_total, c.small_blocks,
		       (unsigned long) c.total, c.blocks,
		       (unsigned long) c.free_total, c.free_blocks,
		       (unsigned long) c.free_max,
		       wlan_heap_alloc_ok, wlan_heap_alloc_fail, wlan_heap_frees,
		       wlan_heap_double_free, wlan_heap_first_fail);
	board_early_print(msg);
}
