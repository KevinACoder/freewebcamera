/*
 * @file   regs.h
 * @brief  Register access primitives for the RK3568 board layer.
 *
 * Not an interface: this is the small private vocabulary the board and driver
 * code share for touching MMIO. It exists because CMSIS has no MMIO API
 * (neither the Driver layer nor the Core layer provides one - the only
 * barriers CMSIS-Core exposes are the __DSB/__ISB/__DMB compiler intrinsics).
 *
 * Two things CMSIS does cover, so they are NOT here:
 *   - interrupts: use irq_ctrl.h (`IRQ_*`), implemented by port/board/gicv3.c
 *   - GPIO lines: use ARM_DRIVER_GPIO, implemented under drivers/
 *
 * Deliberately minimal. Anything not needed by M0 is not defined - interfaces
 * should be added when a real consumer exists, not speculatively.
 *
 * BARRIERS ARE NOT IMPLIED. These are plain accesses; a "write then read back
 * to confirm" sequence needs an explicit barrier between the two. Do not read
 * them as Linux writel/readl.
 */

#ifndef FREEWEBCAMERA_BOARD_REGS_H
#define FREEWEBCAMERA_BOARD_REGS_H

#include <stdint.h>

static inline uint32_t reg_rd32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void reg_wr32(uintptr_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static inline uint8_t reg_rd8(uintptr_t addr)
{
	return *(volatile uint8_t *)addr;
}

static inline void reg_wr8(uintptr_t addr, uint8_t value)
{
	*(volatile uint8_t *)addr = value;
}

static inline void reg_wr64(uintptr_t addr, uint64_t value)
{
	*(volatile uint64_t *)addr = value;
}

/* Read-modify-write. Required for registers where a plain store clears
 * unrelated live bits: GICD_CTLR on this board is the canonical case (a plain
 * store drops ARE, after which every IAR1 read returns spurious INTID 1023). */
static inline void reg_wr32_masked(uintptr_t addr, uint32_t mask, uint32_t value)
{
	reg_wr32(addr, (reg_rd32(addr) & ~mask) | (value & mask));
}

/* Barriers. Prefer the CMSIS intrinsics where they are already in scope;
 * these wrappers exist so board code does not have to include CMSIS-Core. */
static inline void reg_dsb(void)
{
	__asm__ __volatile__("dsb sy" ::: "memory");
}

static inline void reg_isb(void)
{
	__asm__ __volatile__("isb" ::: "memory");
}

#endif /* FREEWEBCAMERA_BOARD_REGS_H */
