/*
 * @file   mmu.c
 * @brief  Identity-mapped MMU bring-up for the RK3568 carrier.
 *
 * Runs at EL1 with the MMU and both caches ON. Normal cacheable memory for
 * the image (unaligned access must work once C library code runs) and
 * Device-nGnRE for every peripheral frame.
 *
 * Identity mapping is deliberate: physical equals virtual, so bare-metal MMIO
 * pointers stay valid and there is no translation layer to debug while
 * bringing up the interrupt controller and the ITS.
 *
 * Layout: 4KiB granule, 48-bit VA (T0SZ=16), walk starts at L0, so L0 entries
 * are always table descriptors, L1 blocks are 1GiB and L2 blocks 2MiB.
 *
 *   L1[0]  0x00000000-0x3fffffff -> L2: 2MiB blocks, Device below the image
 *          RAM base, Normal from 0x0a000000 upward
 *   L1[1]  0x40000000-0x7fffffff -> 1GiB Normal block
 *   L1[2]  0x80000000-0xbfffffff -> 1GiB Normal block
 *   L1[3]  0xc0000000-0xffffffff -> L2: 2MiB blocks, Device everywhere EXCEPT
 *          0xc0000000-0xc1ffffff, which is Normal
 *
 * That last exception matters. The ITS LPI property and pending tables live
 * at 0xc0010000 / 0xc0020000 (see carriers/gicv3_its.c). They are ordinary
 * shared memory that the CPU writes and the ITS reads, so they must be
 * Normal cacheable for the flush/invalidate discipline to mean anything. If
 * they were mapped Device, writes would bypass the cache and the
 * flush/invalidate calls would become no-ops that merely look correct.
 */

#include <stdint.h>

#include "board.h"

/* MAIR attribute indices, referenced from the table descriptors below. */
#define ATTR_IDX_NORMAL		0	/* MAIR byte 0: Normal write-back */
#define ATTR_IDX_DEVICE		1	/* MAIR byte 1: Device-nGnRE */

#define DESC_TABLE		(3ULL << 0)	/* points at a next-level table */
#define DESC_BLOCK		(1ULL << 0)	/* 1GiB (L1) or 2MiB (L2) block */
#define DESC_AF			(1ULL << 10)	/* Access Flag: set, else first
						 * access faults */
#define DESC_SH_INNER		(3ULL << 8)	/* inner shareable */
#define DESC_SH_NONE		(0ULL << 8)	/* non-shareable: Device only */
#define DESC_ATTR_NORMAL	((uint64_t)ATTR_IDX_NORMAL << 2)
#define DESC_ATTR_DEVICE	((uint64_t)ATTR_IDX_DEVICE << 2)
#define DESC_XN			(1ULL << 54)	/* execute-never */

#define PAGE_GRANULE		4096U
#define L2_BLOCK_SIZE		(2ULL * 1024 * 1024)
#define L1_BLOCK_SIZE		(1ULL * 1024 * 1024 * 1024)

/* Image RAM base: below this is firmware and must stay Device (never cached,
 * never speculated, never executed). */
#define IMAGE_RAM_BASE		0x0a000000ULL

/* Normal-cacheable window reserved for the ITS LPI tables. Must agree with
 * LPI_PROP_BASE / LPI_PEND_BASE in carriers/gicv3_its.c. */
#define LPI_TABLE_BASE		0xc0000000ULL
#define LPI_TABLE_END		0xc2000000ULL

/* TCR_EL1 geometry. T0SZ=16 gives 48-bit VA. IRGN0/ORGN0 = write-back
 * write-allocate, SH0 = inner shareable. PS=0b010 selects 40-bit PA, which is
 * what this SoC implements (a larger IPS than the hardware supports is
 * UNPREDICTABLE). */
#define TCR_T0SZ		(16ULL)
#define TCR_IRGN0_WBWA		(3ULL << 8)
#define TCR_ORGN0_WBWA		(3ULL << 10)
#define TCR_SH0_INNER		(3ULL << 12)
#define TCR_PS_40BIT		(2ULL << 32)
#define TCR_EPD1_DISABLE	(1ULL << 23)	/* we only use TTBR0 */

/* Four tables: one per level of the walk.
 *
 * WALK GEOMETRY - getting this wrong makes the board go silent.
 *
 * 4KiB granule with T0SZ=16 is a 48-bit VA, which means a FOUR-level walk
 * starting at L0:
 *
 *   L0  bits [47:39]   only TABLE descriptors are legal
 *   L1  bits [38:30]   1GiB blocks allowed
 *   L2  bits [29:21]   2MiB blocks allowed
 *   L3  bits [20:12]   4KiB pages allowed
 *
 * An earlier revision omitted L0 and placed 1GiB blocks in the L0 table. L0
 * admits no blocks, so the descriptor was invalid: the first instruction
 * fetched after enabling the MMU raised a translation fault and jumped to the
 * fault vector, which parked before the console existed. The image loaded,
 * ran, and printed nothing at all - the characteristic symptom of a bad page
 * table. */
static uint64_t l0_table[512] __attribute__((aligned(PAGE_GRANULE)));
static uint64_t l1_table[512] __attribute__((aligned(PAGE_GRANULE)));
static uint64_t l2_low[512]  __attribute__((aligned(PAGE_GRANULE)));
static uint64_t l2_top[512]  __attribute__((aligned(PAGE_GRANULE)));

/* Normal memory: inner-shareable, cacheable (MAIR byte 0). */
static uint64_t block_desc(uint64_t base, uint64_t attrs)
{
	return base | DESC_BLOCK | DESC_AF | DESC_SH_INNER | attrs;
}

/* Device memory: NON-shareable (MAIR byte 1), execute-never.
 *
 * The shareability field is not decoration. The ARM ARM states that Device
 * memory is Non-shareable, and that a shareable Device mapping is
 * UNPREDICTABLE. Cortex-A55 tolerates it often enough to boot, which is what
 * makes it dangerous: a peripheral register whose status read comes back stale
 * is indistinguishable from a peripheral that has stopped responding, and a
 * polled console that never sees its "transmit room" bit hangs the whole
 * system silently. Every peripheral frame below is mapped through here. */
static uint64_t device_block_desc(uint64_t base)
{
	return base | DESC_BLOCK | DESC_AF | DESC_SH_NONE |
	       DESC_ATTR_DEVICE | DESC_XN;
}

void board_mmu_enable(void)
{
	uint64_t value;
	uint32_t i;

	/* L2 covering the low 1GiB, one 2MiB block per entry. */
	for (i = 0; i < 512U; i++) {
		uint64_t base = (uint64_t)i * L2_BLOCK_SIZE;

		if (base >= IMAGE_RAM_BASE) {
			l2_low[i] = block_desc(base, DESC_ATTR_NORMAL);
		} else {
			l2_low[i] = device_block_desc(base);
		}
	}

	/* L2 covering the top 1GiB. Device by default, Normal for the LPI
	 * table window. */
	for (i = 0; i < 512U; i++) {
		uint64_t base = L1_BLOCK_SIZE * 3ULL + (uint64_t)i * L2_BLOCK_SIZE;

		if (base >= LPI_TABLE_BASE && base < LPI_TABLE_END) {
			l2_top[i] = block_desc(base, DESC_ATTR_NORMAL);
		} else {
			l2_top[i] = device_block_desc(base);
		}
	}

	/* L1 covering the low 4GiB. */
	for (i = 0; i < 512U; i++) {
		l1_table[i] = 0;
	}
	l1_table[0] = DESC_TABLE | (uint64_t)(uintptr_t)l2_low;
	l1_table[1] = block_desc(L1_BLOCK_SIZE,
				 DESC_ATTR_NORMAL);
	l1_table[2] = block_desc(L1_BLOCK_SIZE * 2ULL,
				 DESC_ATTR_NORMAL);
	l1_table[3] = DESC_TABLE | (uint64_t)(uintptr_t)l2_top;

	/* L0: where the walk actually starts for a 48-bit VA. Only table
	 * descriptors are valid at this level. */
	for (i = 0; i < 512U; i++) {
		l0_table[i] = 0;
	}
	l0_table[0] = DESC_TABLE | (uint64_t)(uintptr_t)l1_table;

	/* MAIR: byte 0 Normal write-back read/write-allocate, byte 1
	 * Device-nGnRE. Values match the 4-bit encodings in the ARM ARM. */
	value = 0xffULL | (0x04ULL << 8);
	__asm__ __volatile__("msr mair_el1, %0" : : "r"(value) : "memory");

	value = TCR_T0SZ | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA |
		TCR_SH0_INNER | TCR_PS_40BIT | TCR_EPD1_DISABLE;
	__asm__ __volatile__("msr tcr_el1, %0" : : "r"(value) : "memory");

	/* TTBR0 points at L0, not L1: with T0SZ=16 the table walk base is the
	 * level-0 table. Pointing it one level short makes every access
	 * translate through invalid descriptors. */
	value = (uint64_t)(uintptr_t)l0_table;
	__asm__ __volatile__("msr ttbr0_el1, %0" : : "r"(value) : "memory");

	/* TTBR0 bits[2:0] must be zero for a 4KiB granule; the table is
	 * aligned, so this holds. A mismatch here faults on the first
	 * instruction fetch after enabling the MMU. */

	/* Last chance to drop the loader's dirty lines over our own memory:
	 * this runs with caching off, so nothing here is cached yet, and any
	 * line the loader left behind would otherwise be written back over the
	 * tables we just built (and the code, and the stack) once the caches
	 * come on. */
	board_cache_init();

	__asm__ __volatile__("dsb sy" ::: "memory");
	__asm__ __volatile__("isb" ::: "memory");

	/* Turn on MMU (M), data cache (C) and instruction cache (I). */
	__asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(value));
	value |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
	__asm__ __volatile__("msr sctlr_el1, %0" : : "r"(value) : "memory");
	__asm__ __volatile__("isb" ::: "memory");
}
