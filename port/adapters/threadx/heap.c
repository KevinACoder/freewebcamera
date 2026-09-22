/*
 * @file   heap.c
 * @brief  Kernel-free static heap for the ThreadX line.
 *
 * The FreeRTOS image's allocation surface comes from the kernel's heap_4
 * running over ucHeap (port/adapters/freertos/heap.c). ThreadX has no
 * integrator heap at all - its tx_byte_allocate needs a pool somebody has
 * to create, and consumers would rather not bootstrap one - so this file
 * is the ThreadX twin: one static region, an address-ordered block list
 * with coalescing on free, and the pvPortMalloc/vPortFree names the
 * in-tree consumers already call (the thread stacks in cmsis_os2_impl.c,
 * the sdmmc OSA layer, CherryUSB's osal).
 *
 * Protection is _tx_thread_smp_protect / _tx_thread_smp_unprotect - the
 * primitive the kernel's own TX_DISABLE expands to on this port (the
 * cortex_a55_smp port implements them in assembly over a global
 * test-and-set; the save value is the caller's DAIF). That makes the heap
 * correct before the kernel starts - the deferred thread-creation window
 * allocates stacks here, with IRQs already enabled and no scheduler yet -
 * and nestable from any core.
 *
 * The region lives in .bss, which mmu.c maps as Normal cacheable memory;
 * heap metadata does unaligned-unfriendly word stores and would trap in
 * Device mappings. Same reasoning as the FreeRTOS twin's ucHeap.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern void board_early_print(const char *s);

/* The port assembly's protection primitives (tx_thread_smp_protect.S /
 * tx_thread_smp_unprotect.S), spelled out to keep this file free of kernel
 * headers; UINT on this port is unsigned int. */
extern unsigned int _tx_thread_smp_protect(void);
extern void _tx_thread_smp_unprotect(unsigned int save);

#define HEAP_BYTES	(1024u * 1024u)

/* Every block - allocated or free - stays threaded in one address-ordered
 * list. That makes free-side coalescing a neighbor check instead of a
 * boundary-tag decode, at the cost of a header per live allocation; the
 * allocation counts here (thread stacks, adapter structures) are tens, not
 * thousands, so the header tax is noise next to a simpler invariant. */
typedef struct heap_block {
	struct heap_block *next;
	struct heap_block *prev;
	uint32_t           size;	/* payload bytes, always 16-aligned */
	uint8_t            used;
} heap_block_t;

static uint8_t heap_region[HEAP_BYTES] __attribute__((aligned(16)));
static heap_block_t heap_head;		/* sentinel; heap never empty after init */
static uint8_t heap_ready;
static uint32_t heap_free_bytes = HEAP_BYTES;

#define HDR_SIZE	(sizeof(heap_block_t))
#define ALIGN		16u
#define MIN_PAYLOAD	ALIGN

static void heap_init(void)
{
	heap_block_t *first = (heap_block_t *)heap_region;

	heap_head.next = first;
	heap_head.prev = first;
	first->next = &heap_head;
	first->prev = &heap_head;
	first->size = HEAP_BYTES - HDR_SIZE;
	first->used = 0u;
	heap_free_bytes = HEAP_BYTES - HDR_SIZE;
	heap_ready = 1u;
}

void *pvPortMalloc(size_t length)
{
	uint8_t *payload = NULL;
	unsigned int save;
	heap_block_t *blk;
	uint32_t want;

	if (length == 0u || length > HEAP_BYTES) {
		return NULL;
	}
	want = ((uint32_t)length + (ALIGN - 1u)) & ~(ALIGN - 1u);
	if (want < MIN_PAYLOAD) {
		want = MIN_PAYLOAD;
	}

	save = _tx_thread_smp_protect();
	if (!heap_ready) {
		heap_init();
	}
	for (blk = heap_head.next; blk != &heap_head; blk = blk->next) {
		uint32_t remain;
		heap_block_t *split;

		if (blk->used || blk->size < want) {
			continue;
		}
		remain = blk->size - want;
		if (remain >= HDR_SIZE + MIN_PAYLOAD) {
			/* Carve the tail into a new free block; splitting the
			 * tail (not the head) keeps the list address-ordered
			 * without a re-sort. */
			split = (heap_block_t *)((uint8_t *)blk + HDR_SIZE + want);
			split->size = remain - HDR_SIZE;
			split->used = 0u;
			split->prev = blk;
			split->next = blk->next;
			blk->next->prev = split;
			blk->next = split;
			heap_free_bytes -= HDR_SIZE;
			blk->size = want;
		} else {
			heap_free_bytes -= blk->size;
		}
		blk->used = 1u;
		payload = (uint8_t *)blk + HDR_SIZE;
		break;
	}
	_tx_thread_smp_unprotect(save);
	return payload;
}

void vPortFree(void *ptr)
{
	unsigned int save;
	heap_block_t *blk;

	if (ptr == NULL) {
		return;
	}
	blk = (heap_block_t *)((uint8_t *)ptr - HDR_SIZE);

	/* M7 hardening: a free of a pointer that is not this heap's payload
	 * (interior pointer, double free of a foreign allocation, wild
	 * pointer) used to be an un.logged write into foreign memory that
	 * surfaced as a wild jump far away from the cause. Validate the
	 * block before touching it; refuse and report instead. */
	/* NB: payload is blk + 24 on LP64, so pointers are 8 mod 16 here -
	 * no alignment check on ptr, only bounds and header sanity. */
	if ((uint8_t *)blk < heap_region ||
	    (uint8_t *)blk > heap_region + HEAP_BYTES - HDR_SIZE ||
	    blk->size > HEAP_BYTES || blk->used == 0u) {
		static char bad_msg[64];

		(void)snprintf(bad_msg, sizeof(bad_msg),
			       "heap: BAD FREE %08lx (refused)\n",
			       (unsigned long)(uintptr_t)ptr);
		board_early_print(bad_msg);
		return;
	}

	save = _tx_thread_smp_protect();
	blk->used = 0u;
	heap_free_bytes += blk->size;

	/* Coalesce forward, then backward: with the list address-ordered the
	 * neighbors are the only merge candidates. */
	if (blk->next != &heap_head && !blk->next->used) {
		heap_block_t *nxt = blk->next;

		blk->size += HDR_SIZE + nxt->size;
		blk->next = nxt->next;
		nxt->next->prev = blk;
		heap_free_bytes += HDR_SIZE;
	}
	if (blk->prev != &heap_head && !blk->prev->used) {
		heap_block_t *prv = blk->prev;

		prv->size += HDR_SIZE + blk->size;
		prv->next = blk->next;
		blk->next->prev = prv;
		heap_free_bytes += HDR_SIZE;
	}
	_tx_thread_smp_unprotect(save);
}

size_t xPortGetFreeHeapSize(void)
{
	unsigned int save;
	size_t free_bytes;

	save = _tx_thread_smp_protect();
	free_bytes = heap_free_bytes;
	_tx_thread_smp_unprotect(save);
	return free_bytes;
}
