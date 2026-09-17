/*
 * @file   mmu.c
 * @brief  Identity-mapped MMU bring-up for the board layer.
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
 * are always table descriptors, L1 blocks are 1GiB and L2 blocks 2MiB. The
 * addresses come from board_conf.h:
 *
 *   L1[0]  0x00000000-0x3fffffff -> L2: 2MiB blocks, Device below the image
 *          RAM base (BOARD_MMU_IMAGE_RAM_BASE), Normal above it
 *   L1[1]  0x40000000-0x7fffffff -> 1GiB Normal block
 *   L1[2]  0x80000000-0xbfffffff -> 1GiB Normal block
 *   L1[3]  0xc0000000-0xffffffff -> L2: 2MiB blocks, Device everywhere EXCEPT
 *          the BOARD_MMU_LPI_WINDOW range, which is Normal
 *   L1[L1_IDX_PCIE_DBI] (the 1GiB holding the PCIe DBI frames) -> L2: Device,
 *          only those frames populated; present only when the board conf
 *          defines BOARD_MMU_PCIE_DBI0_BASE
 *
 * The L1[3] Normal window matters (or did): the ITS LPI property and pending
 * tables are ordinary shared memory that the CPU writes and the ITS reads, so
 * they must be Normal cacheable for the flush/invalidate discipline to mean
 * anything. If they were mapped Device, writes would bypass the cache and the
 * flush/invalidate calls would become no-ops that merely look correct. (The
 * tables have since moved into the image's .bss, inside the Normal image RAM
 * mapping; the conf window is kept because removing it would change the boot
 * mapping for no measured benefit.)
 *
 * The PCIe DBI entry exists because the DesignWare register file (DBI, where
 * the iATU lives and where the root port's own config space is served) is
 * addressed above 4GiB on RK3568. Those frames are the only thing up there, so
 * only they are populated - an unmapped access faults loudly instead of
 * landing on a Device mapping that happens to read as zero.
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

/* Image RAM base (board_conf.h): below this is firmware and must stay Device
 * (never cached, never speculated, never executed). */
#define IMAGE_RAM_BASE		BOARD_MMU_IMAGE_RAM_BASE

/* Normal-cacheable window inside the top 1GiB (board_conf.h). An empty
 * window (BASE == END) maps the whole 1GiB Device. */
#define LPI_TABLE_BASE		BOARD_MMU_LPI_WINDOW_BASE
#define LPI_TABLE_END		BOARD_MMU_LPI_WINDOW_END

/* DesignWare PCIe register files (DBI), one per controller, both in the same
 * 1GiB - the RK3568 numbers, matching the board table in
 * drivers/rk3568_pcie.c. Boards without PCIe above 4GiB do not define
 * BOARD_MMU_PCIE_DBI0_BASE and get no L2 table for it. */
#ifdef BOARD_MMU_PCIE_DBI0_BASE
#define PCIE_DBI0_BASE		BOARD_MMU_PCIE_DBI0_BASE
#define PCIE_DBI0_END		BOARD_MMU_PCIE_DBI0_END
#define PCIE_DBI1_BASE		BOARD_MMU_PCIE_DBI1_BASE
#define PCIE_DBI1_END		BOARD_MMU_PCIE_DBI1_END
#define L1_IDX_PCIE_DBI		((uint32_t)((PCIE_DBI0_BASE >> 30) & 0x1ffULL))
#endif

/* TCR_EL1 geometry. T0SZ=16 gives 48-bit VA. IRGN0/ORGN0 = write-back
 * write-allocate, SH0 = inner shareable. The physical address size is the
 * board's (BOARD_MMU_TCR_PS): a larger IPS than the hardware supports is
 * UNPREDICTABLE. */
#define TCR_T0SZ		(16ULL)
#define TCR_IRGN0_WBWA		(3ULL << 8)
#define TCR_ORGN0_WBWA		(3ULL << 10)
#define TCR_SH0_INNER		(3ULL << 12)
#define TCR_PS_40BIT		BOARD_MMU_TCR_PS
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
#ifdef BOARD_MMU_PCIE_DBI0_BASE
static uint64_t l2_pcie[512] __attribute__((aligned(PAGE_GRANULE)));
#endif

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

	/* L2 covering the 1GiB that holds the two PCIe DBI register files.
	 * Only those two frames get a descriptor; the rest of the 1GiB stays
	 * invalid on purpose. */
#ifdef BOARD_MMU_PCIE_DBI0_BASE
	for (i = 0; i < 512U; i++) {
		uint64_t base = (uint64_t)L1_IDX_PCIE_DBI * L1_BLOCK_SIZE +
				(uint64_t)i * L2_BLOCK_SIZE;

		if ((base >= PCIE_DBI0_BASE && base < PCIE_DBI0_END) ||
		    (base >= PCIE_DBI1_BASE && base < PCIE_DBI1_END)) {
			l2_pcie[i] = device_block_desc(base);
		} else {
			l2_pcie[i] = 0;
		}
	}
#endif

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
#ifdef BOARD_MMU_PCIE_DBI0_BASE
	l1_table[L1_IDX_PCIE_DBI] = DESC_TABLE | (uint64_t)(uintptr_t)l2_pcie;
#endif

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

/* Secondary-core variant: point THIS core at the boot core's page tables and
 * switch its MMU on. Deliberately NOT board_mmu_enable(), and the difference
 * is the whole point:
 *
 *  - No table build. The tables are plain Normal memory already in DRAM (the
 *    boot core wrote them with its caches still off), and any core that sets
 *    TTBR0 to the L0 table walks them. Rebuilding them here - clearing each
 *    level before rewriting - would, for a few microseconds, leave descriptors
 *    invalid while the BOOT CORE is translating every instruction and data
 *    access through those same tables: its walk hits the cleared entries and
 *    it dies on a synchronous exception through its pre-scheduler vectors.
 *    That is not hypothetical; it is exactly what the first SMP boot did.
 *
 *  - No whole-image cache flush (board_cache_init). That job exists to drain
 *    the LOADER's dirty lines before our caches come on; a secondary has no
 *    such lines, and flushing the image from a second core while the boot
 *    core mutates it is a lost-update race (clean, then invalidate, while the
 *    owner re-dirties between the two halves).
 *
 *  - MAIR/TCR/TTBR0/SCTLR are per-core registers: every core writes its own,
 *    with the same values. Instruction cache is invalidated locally (ic
 *    iallu) so no stale pre-MMU fetches survive. Everything shared after
 *    both MMUs are on is inner-shareable inside one coherent cluster. */
void board_mmu_enable_secondary(void)
{
	uint64_t value;

	value = 0xffULL | (0x04ULL << 8);
	__asm__ __volatile__("msr mair_el1, %0" : : "r"(value) : "memory");

	value = TCR_T0SZ | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA |
		TCR_SH0_INNER | TCR_PS_40BIT | TCR_EPD1_DISABLE;
	__asm__ __volatile__("msr tcr_el1, %0" : : "r"(value) : "memory");

	value = (uint64_t)(uintptr_t)l0_table;
	__asm__ __volatile__("msr ttbr0_el1, %0" : : "r"(value) : "memory");

	/* Drop anything this core speculatively fetched before the MMU was
	 * configured, then enable M|C|I. */
	__asm__ __volatile__("ic iallu" ::: "memory");
	__asm__ __volatile__("dsb sy" ::: "memory");
	__asm__ __volatile__("isb" ::: "memory");

	__asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(value));
	value |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
	__asm__ __volatile__("msr sctlr_el1, %0" : : "r"(value) : "memory");
	__asm__ __volatile__("isb" ::: "memory");
}
