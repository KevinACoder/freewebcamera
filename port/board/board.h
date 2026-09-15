/*
 * @file   board.h
 * @brief  Board bring-up for the RK3568: everything needed to get the CPU
 *         into a state where the CMSIS interfaces can work.
 *
 * This is not an interface layer and not a driver. It holds the code that
 * runs before any of that exists:
 *
 *   startup.S   EL2/EL3 -> EL1, vector tables, .bss, then C
 *   mmu.c       identity map with caches on
 *   gicv3.c     the backend behind CMSIS irq_ctrl.h (`IRQ_*`)
 *   tick.c      the RTOS tick, in the CMSIS OS_Tick_* shape
 *
 * Two deliberate design points:
 *
 *  - Interrupts are NOT invented here. The API is CMSIS irq_ctrl.h, and
 *    gicv3.c is merely its implementation. Callers use IRQ_* and never see
 *    this header.
 *
 *  - The tick is exposed as OS_Tick_* (also CMSIS), so this layer carries no
 *    FreeRTOS dependency: swapping the kernel must not require editing it.
 */

#ifndef FREEWEBCAMERA_BOARD_H
#define FREEWEBCAMERA_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "os_tick.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- image entry (called from startup.S) --------------------------------- */

/* Identity-mapped MMU with caches on. Must run before any C code that relies
 * on unaligned Normal access or performs cache maintenance. */
void board_mmu_enable(void);

/* C entry point of the image. */
void board_main(void);

/* --- board coordinates (register facts, verified on this board) ---------- */

#define BOARD_UART2_BASE	0xfe660000UL
#define BOARD_GICD_BASE		0xfd400000UL
#define BOARD_GICR_BASE		0xfd460000UL	/* core 0 redistributor */
#define BOARD_ITS_BASE		0xfd440000UL
#define BOARD_ITS_TRANSLATER	0xfd450040UL

/* --- early output --------------------------------------------------------- */

/* Fatal bring-up failures are reported here. Installed by the console driver
 * once output is possible; before that it is a no-op, which is the correct
 * default (writing to an unprogrammed UART looks like a working console that
 * prints nothing). */
extern void (*board_early_print_hook)(const char *message);

void board_early_print(const char *message);

/* --- GICv3 (implementation of CMSIS irq_ctrl.h) -------------------------- */

/* Bring up distributor, redistributor and CPU interface, in that order.
 * Idempotent: the kernel port calls it again after the boot path already did,
 * and the second call is a no-op.
 *
 * Board facts baked in, each established by experiment here:
 *  - GICD_CTLR is only ever read-modify-written. Firmware boots the
 *    distributor with DS=1 and ARE set (measured 0x12); a plain store of the
 *    enable bits clears ARE, and once ARE is clear the system-register CPU
 *    interface stops working - every ICC_IAR1_EL1 read returns spurious INTID
 *    1023 while the lines keep asserting.
 *  - The redistributor needs the WAKER handshake (clear ProcessorSleep, wait
 *    for ChildrenAsleep) or SPIs and PPIs are never delivered.
 *  - GICR_CTLR is NOT written here: EnableLPIs belongs to the LPI/ITS setup,
 *    and an earlier version that wrote it from a GICD-derived mask wrongly
 *    set EnableLPIs and CES together.
 *  - ICC_IGRPEN1_EL1 is not written at EL1 - a non-secure write resets the
 *    board. Firmware has already enabled Group 1. */
void board_gicv3_init(void);

/* Dispatch entry called by the kernel port's vApplicationIRQHandler with the
 * INTID already acknowledged. Runs in interrupt context. */
void board_gicv3_dispatch(uint32_t intid);

/* --- tick ----------------------------------------------------------------- */

/* Tick source: the EL1 virtual timer (CNTV), INTID 27.
 *
 * Not the physical timer, despite that being the obvious choice. On this
 * board OP-TEE claims the physical timer as a Group 0 interrupt that
 * non-secure code cannot enable, and the EL2 physical timer is unreachable
 * because the `go` boot path leaves CNTHCTL_EL2.EL1PCEN=0. The virtual timer
 * is the one that works, measured: writing CNTV_TVAL raises GICR_ISPENDR0
 * bit 27 and INTID 27 arrives.
 *
 * The OS_Tick_* functions below are declared in include/os_tick.h (CMSIS); a
 * separate declaration would be redundant. */
#define BOARD_TICK_INTID	27U

/* True once the virtual timer is armed and running. */
bool board_tick_is_running(void);

/* --- interrupt priorities (raw hardware values, 4 priority bits) ---------- */
/* The board owns this policy, not the kernel: FreeRTOSConfig.h derives its
 * configMAX_API_CALL_INTERRUPT_PRIORITY from BOARD_IRQ_PRIORITY_API_CALL, so
 * the direction is config -> board rather than app -> config. An application
 * that needs a priority uses these constants and never sees a kernel macro.
 *
 * Smaller numeric value = more urgent (GIC convention).
 *
 * Any interrupt that calls a FromISR API must be at API_CALL: the port asserts
 * this in vPortValidateInterruptPriority. The tick must be at TICK, the lowest
 * usable level: FreeRTOS_Tick_Handler asserts that too. Getting either wrong
 * trips an assertion at run time, which is why they are named here rather than
 * written as literals at call sites. */
#define BOARD_IRQ_PRIORITY_TICK		15U	/* lowest usable */
#define BOARD_IRQ_PRIORITY_API_CALL	11U
#define BOARD_IRQ_PRIORITY_DEFAULT	10U

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_BOARD_H */
