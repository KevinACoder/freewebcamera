/*
 * @file   cache.c
 * @brief  AArch64 data cache maintenance for the board layer.
 *
 * CMSIS-Core has no AArch64 cache API: the only cache helpers the pack ships
 * are in armv7a_cp15.h, which is ARMv7/CP15 and does not apply to an A55.
 * So this is the one genuinely uncovered primitive, and it is kept here in
 * the board layer rather than exposed as an interface.
 *
 * Cache line is 64 bytes on Cortex-A55. Ranges are rounded outward: cleaning
 * or invalidating a few extra lines is harmless, whereas missing one is the
 * silent-failure mode this exists to prevent. That asymmetry is why the
 * rounding direction is always outward and never "aligned down and hope".
 *
 * This has already bitten this project in five places on this board (IRQ,
 * ITS, GMAC, NVMe, EHCI): shared tables are reached through non-coherent
 * ports, so every CPU write needs a clean before the device reads it, and
 * every device write needs an invalidate before the CPU reads it.
 */

#include <stdint.h>

#include "regs.h"

#define DCACHE_LINE	64u

/* op: 'c' clean (write back), 'i' invalidate (discard), 'b' clean+invalidate */
static void dcache_op(uintptr_t addr, unsigned long size, char op)
{
	uintptr_t start = addr & ~(uintptr_t)(DCACHE_LINE - 1u);
	uintptr_t end = (addr + size + DCACHE_LINE - 1u) &
			~(uintptr_t)(DCACHE_LINE - 1u);
	uintptr_t p;

	for (p = start; p < end; p += DCACHE_LINE) {
		switch (op) {
		case 'c':
			__asm__ __volatile__("dc cvac, %0" ::"r"(p) : "memory");
			break;
		case 'i':
			/* ivac discards without writing back - correct only when
			 * the CPU has no dirty data in the range. */
			__asm__ __volatile__("dc ivac, %0" ::"r"(p) : "memory");
			break;
		default:
			__asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
			break;
		}
	}
	__asm__ __volatile__("dsb sy" ::: "memory");
}

void board_dcache_flush(uintptr_t addr, unsigned long size)
{
	if (size != 0u) {
		dcache_op(addr, size, 'c');
	}
}

void board_dcache_invalidate(uintptr_t addr, unsigned long size)
{
	if (size != 0u) {
		dcache_op(addr, size, 'i');
	}
}

void board_dcache_flush_invalidate(uintptr_t addr, unsigned long size)
{
	if (size != 0u) {
		dcache_op(addr, size, 'b');
	}
}
