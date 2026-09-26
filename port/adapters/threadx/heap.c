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
 * cmsis_os2_impl.c, the net80211 adapter, the sdmmc OSA layer).
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

extern void board_early_print(const char *s);

/* The port assembly's protection primitives (tx_thread_smp_protect.S /
 * tx_thread_smp_unprotect.S), spelled out to keep this file free of kernel
 * headers; UINT on this port is unsigned int. */
extern unsigned int _tx_thread_smp_protect(void);
extern void _tx_thread_smp_unprotect(unsigned int save);

/* The iwm line needs the headroom: its RX ring alone hands out 256
 * mbuf+cluster allocations (~1.1 MB) from this heap on top of the
 * USB/net80211 world (the first PCIe board run died silently in
 * m_gethdr at "could not allocate RX ring").  The linker script
 * reserves 4M for this region (bss is NOLOAD), so the reservation is
 * free in the image. */
#define HEAP_BYTES	(4u * 1024u * 1024u)

static uint8_t heap_region[HEAP_BYTES] __attribute__((aligned(32)));
static tlsf_t heap_tlsf;

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

void *pvPortMalloc(size_t length)
{
	uint8_t *payload = NULL;
	unsigned int save;
	uintptr_t ra = (uintptr_t) __builtin_return_address(0);

	if (length == 0u || length > HEAP_BYTES) {
		return NULL;
	}

	save = _tx_thread_smp_protect();
	if (heap_tlsf == (tlsf_t) 0) {
		heap_tlsf = tlsf_create_with_pool(heap_region, HEAP_BYTES);
	}
	if (heap_tlsf != (tlsf_t) 0) {
		payload = tlsf_malloc(heap_tlsf, length);
	}
	if (payload != NULL) {
		heap_ring_push(ra, (uintptr_t) payload, (uint32_t) length, 0);
		heap_check(ra);
	}
	_tx_thread_smp_unprotect(save);
	return payload;
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
	heap_ring_push(ra, (uintptr_t) ptr,
		       (uint32_t) tlsf_block_size(ptr), 1);
	tlsf_free(heap_tlsf, ptr);
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
