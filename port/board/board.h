/*
 * @file   board.h
 * @brief  Board bring-up interface: everything needed to get the CPU into a
 *         state where the CMSIS interfaces can work.
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
 *
 * BOARD SELECTION: the code in common/ is board-agnostic and is calibrated
 * through board_conf.h, which the build resolves from port/board/<board>/
 * (Makefile: BOARD=rk3568 | aarch64-virt). This header carries the shared
 * policy and every function signature; the per-board register bases,
 * interrupt numbers, MMU windows and PSCI conventions live in that board's
 * board_conf.h.
 */

#ifndef FREEWEBCAMERA_BOARD_H
#define FREEWEBCAMERA_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "os_tick.h"

/* The board's calibration table: register bases, interrupt numbers, MMU
 * windows, core-numbering and PSCI conventions. Resolved from
 * port/board/$(BOARD)/ via the include path. */
#include "board_conf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- image entry (called from startup.S) --------------------------------- */

/* Identity-mapped MMU with caches on. Must run before any C code that relies
 * on unaligned Normal access or performs cache maintenance. */
void board_mmu_enable(void);

/* Secondary-core variant: bind this core to the boot core's already-built
 * page tables and turn its own MMU/caches on. Never rebuilds the tables and
 * never flushes the image - see mmu.c for why both would kill the boot
 * core. */
void board_mmu_enable_secondary(void);

/* Drop the loader's leftover dirty cache lines over our own image, then
 * invalidate the instruction cache. Called by board_mmu_enable() while caches
 * are still off; see port/board/common/cache.c for why skipping it produces
 * intermittent corruption rather than a clean failure. */
void board_cache_init(void);

/* Data cache maintenance. CMSIS has no AArch64 cache API, so these are board
 * primitives, not an interface. They matter wherever CPU-written memory is read
 * by a device over a non-coherent port (the ITS property/pending and command
 * tables are the live example): a `dsb` orders a write, it does not write it
 * back, so ordering alone leaves the device looking at stale memory. */
void board_dcache_flush(uintptr_t addr, unsigned long size);
void board_dcache_invalidate(uintptr_t addr, unsigned long size);
void board_dcache_flush_invalidate(uintptr_t addr, unsigned long size);

/* C entry point of the image. */
void board_main(void);

/* Board coordinates - register bases, console/tick interrupt numbers, the
 * redistributor stride, the UART calibration and the MMU windows - are the
 * board_conf.h table above, not repeated here. */

/* --- early output --------------------------------------------------------- */

/* Fatal bring-up failures are reported here. Installed by the console driver
 * once output is possible; before that it is a no-op, which is the correct
 * default (writing to an unprogrammed UART looks like a working console that
 * prints nothing). */
extern void (*board_early_print_hook)(const char *message);

void board_early_print(const char *message);

/* Same sink, formatted. Drivers that report what they found (register
 * versions, PHY ids, negotiated link speed) use this instead of each carrying
 * its own formatter. One line per call; not for per-packet output. */
void board_log(const char *fmt, ...);

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
 *  - GICD_IPRIORITYR is one byte per INTID at 0x400 + id (no stride), and the
 *    redistributor's per-INTID registers live in the SGI_base frame at
 *    RD_base + 0x10000. Getting either of those addresses wrong does not fault
 *    - it resets the SoC or silently fails to enable the interrupt.
 *  - The redistributor needs the WAKER handshake (clear ProcessorSleep, wait
 *    for ChildrenAsleep) or SPIs and PPIs are never delivered.
 *  - A PPI needs both IGROUPR0 bit set and IGRPMODR0 bit clear to be Group 1
 *    non-secure; setting IGROUPR0 alone selects Group 1 *Secure*, which a
 *    non-secure handler never receives.
 *  - ICC_IGRPEN1_EL1 is enabled here if it is not already on. The board-proven
 *    GICv3 path for this SoC writes it; skipping it leaves every Group 1
 *    interrupt undelivered while all distributor state reads back correct.
 *  - GICR_CTLR is NOT written: EnableLPIs belongs to the LPI/ITS setup. */
void board_gicv3_init(void);

/* Dispatch entry called by the kernel port's vApplicationIRQHandler with the
 * INTID already acknowledged. Runs in interrupt context. */
void board_gicv3_dispatch(uint32_t intid);

/* --- tick ----------------------------------------------------------------- */

/* The tick INTID lives in board_conf.h (BOARD_TICK_INTID); on every board so
 * far it is the EL1 virtual timer, INTID 27 - the physical-timer alternatives
 * are firmware-hostage on real hardware, and the RK3568 story is told in that
 * board's conf. */

/* True once the virtual timer is armed and running. */
bool board_tick_is_running(void);

/* --- console -------------------------------------------------------------- */

/* The console UART's GIC INTID is BOARD_CONSOLE_INTID (board_conf.h). */

/* --- SMP ------------------------------------------------------------------- */

/* The number of cores, the redistributor frame stride, how a core's logical
 * number is extracted from MPIDR, and the PSCI conventions (conduit
 * instruction, CPU_ON target encoding) are board_conf.h policy. The
 * declarations below are the board-independent SMP surface every board
 * provides. */

#define BOARD_SMP_CORES		4U

/* Cross-core yield interrupt: SGI 0, chosen by the kernel port (portmacro.h).
 * Declared here because the priority policy lives with the board's other
 * interrupt priorities; the board does not get to move the number. */
#define BOARD_SMP_YIELD_INTID	0U

/* Logical core number of the calling core.
 *
 * The extraction (which MPIDR affinity field carries the logical number) is
 * BOARD_MPIDR_CORE_SHIFT in board_conf.h - RK3568 numbers cores in Aff1,
 * QEMU virt in Aff0, and getting that wrong made every secondary believe it
 * was core 0 (bring-up rounds 2-5 on RK3568). Everything that derives a core
 * identity reads it through this function so the numbering has exactly one
 * home - including the assembly paths, whose macro expands to the same shift
 * from the same conf header. */
static inline uint32_t board_smp_core_id(void)
{
	uint64_t mpidr;

	__asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
	return (uint32_t)((mpidr >> BOARD_MPIDR_CORE_SHIFT) & 0xffu);
}

/* Release cores 1..BOARD_SMP_CORES-1 through PSCI CPU_ON, targeting the
 * secondary entry point in smp_secondary.S, then wait (bounded) until each
 * core has run its GIC bring-up and reported in.
 *
 * Called from TASK context after the scheduler is running (the app's boot
 * task does it), not from the kernel port: releasing three cores while the
 * boot core is still alone in its pre-ticker window produced freezes with no
 * evidence on RK3568. This way a straggling core costs one core, not the
 * boot. */
void board_smp_start_secondaries(void);

/* A secondary core calls this once its redistributor / CPU interface / SGI
 * setup is done. The boot core's bounded wait in
 * board_smp_start_secondaries() is waiting for exactly these reports. */
void board_smp_mark_core_up(uint32_t core);

/* How many cores have reported up so far (boot-anchor evidence). */
uint32_t board_smp_up_count(void);

/* PSCI VERSION through the SMC conduit, 0 when BL31 does not answer. The
 * composite form 0xMMmmmm (e.g. 0x10001 = PSCI 1.1). */
uint32_t board_smp_psci_version(void);

/* GICv3 per-core additions for SMP. board_gicv3_secondary_init() runs ON a
 * secondary core and brings up ITS OWN redistributor and CPU interface (the
 * distributor stays a boot-core-only concern); board_gicv3_send_sgi() raises
 * a software interrupt on the cores in core_mask (bit i = logical core i). */
void board_gicv3_secondary_init(void);
void board_gicv3_send_sgi(uint32_t intid, uint32_t core_mask);

/* --- interrupt priorities ------------------------------------------------- */
/* The board owns this policy, not the kernel: FreeRTOSConfig.h derives its
 * configMAX_API_CALL_INTERRUPT_PRIORITY from BOARD_IRQ_PRIORITY_API_CALL, so
 * the direction is config -> board rather than app -> config. An application
 * that needs a priority uses these constants and never sees a kernel macro.
 *
 * Smaller numeric value = more urgent (GIC convention).
 *
 * Two levels of value are in play and mixing them up is silent:
 *
 *   - The constants below are LOGICAL levels, 0..15, the same numbering the
 *     kernel port uses (configUNIQUE_INTERRUPT_PRIORITIES == 16).
 *   - The GIC register holds them left-justified in the top 4 bits of a byte,
 *     so a logical level N is written as N << BOARD_IRQ_PRIORITY_SHIFT. The
 *     *_RAW constants below do that shift, and IRQ_SetPriority (CMSIS
 *     irq_ctrl.h) takes the raw byte.
 *
 * Writing a raw level as if it were logical (0xf0 for level 15) produces a
 * different hardware level and trips the port's assertions. Always pass the
 * *_RAW form to IRQ_SetPriority.
 *
 * Any interrupt that calls a FromISR API must be at API_CALL: the port asserts
 * this in vPortValidateInterruptPriority. The tick must be at TICK: the port's
 * FreeRTOS_Tick_Handler asserts the *lowest usable* level, which is one above
 * the absolute lowest, because the port reserves that for itself. Getting
 * either wrong trips an assertion at run time, which is why they are named
 * here rather than written as literals at call sites. */
#define BOARD_IRQ_PRIORITY_SHIFT	4U
#define BOARD_IRQ_PRIORITY_TICK		14U	/* lowest usable, not 15 */
#define BOARD_IRQ_PRIORITY_API_CALL	11U
#define BOARD_IRQ_PRIORITY_DEFAULT	10U

/* The cross-core yield SGI. Above API_CALL (numerically lower) on purpose:
 * the SGI must stay deliverable while a core sits in a kernel critical
 * section with ICC_PMR narrowed to API_CALL, or cross-core preemption stalls
 * for the whole critical section. Its handler only sets a per-core "yield
 * requested" flag and never calls a kernel API, so running above the API-call
 * level breaks no port assertion. */
#define BOARD_IRQ_PRIORITY_SGI		9U

#define BOARD_IRQ_PRIORITY_TICK_RAW \
	(BOARD_IRQ_PRIORITY_TICK << BOARD_IRQ_PRIORITY_SHIFT)
#define BOARD_IRQ_PRIORITY_API_CALL_RAW \
	(BOARD_IRQ_PRIORITY_API_CALL << BOARD_IRQ_PRIORITY_SHIFT)
#define BOARD_IRQ_PRIORITY_DEFAULT_RAW \
	(BOARD_IRQ_PRIORITY_DEFAULT << BOARD_IRQ_PRIORITY_SHIFT)
#define BOARD_IRQ_PRIORITY_SGI_RAW \
	(BOARD_IRQ_PRIORITY_SGI << BOARD_IRQ_PRIORITY_SHIFT)

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_BOARD_H */
