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
 * are still off; see port/aarch64/cache.c for why skipping it produces
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

/* Project log convention (NetBSD dmesg shape): one line, one stamp,
 * "module: message". Every line through board_early_print / board_log is
 * prefixed "[   s.mmm] " - boot-relative seconds from CNTVCT, captured at
 * the first stamped print - and names its module in the message ("uart: ...",
 * "smp: ...", "fatal: ..."). The shell's interactive echo/response stays on
 * the CMSIS console driver and is not stamped; the two sinks may interleave
 * on the wire, which is cosmetic.
 *
 * board_early_print / board_log write on the POLLED early UART
 * (startup.S's uart_early_puts) under a per-line spinlock. They never block
 * and are safe from any task context, on any core. */
void board_early_print(const char *message);

/* Unstamped but LOCKED write on the same polled UART: the sink for console
 * frontends that must not garble against board_early_print (the net80211
 * world's printf used to drive the CMSIS USART driver on a different lock
 * domain, and attach-time prints shredded fault dumps mid-line). */
void board_console_write(const char *message);

/* Same sink, formatted. Drivers that report what they found (register
 * versions, PHY ids, negotiated link speed) use this instead of each carrying
 * its own formatter. One line per call; not for per-packet output. */
void board_log(const char *fmt, ...);

/* gdb-session console gate (D56). While a stub session is live, the locked
 * sinks (board_early_print / board_log) drop their output instead of
 * spraying task logs into the middle of the RSP stream; what would have
 * been printed is counted, and the stub flushes a one-line accounting when
 * the session ends. The mute ask is weak-linked: the images without the
 * stub (FreeRTOS, ThreadX SMP, ktest) never see a session, and the stub's
 * glue supplies the strong definition. */
void board_console_gate_report(void);

/* Console RX break-in seam: the shell and the gdb stub share the one UART,
 * so the shell's RX path offers every 0x03 byte here BEFORE handing it to
 * the shell. The weak default answers 0 and the byte is delivered to the
 * shell as ordinary input; the stub's glue (tx_gdb_glue.c) overrides it,
 * consumes the byte and raises the BRK that enters a debug session.
 * Returns 1 when the byte was consumed. */
int board_console_break_hook(void);

/* Stamped but deliberately NOT locked: for contexts that must not spin on
 * the print lock (ISRs, the SMP bring-up window, the secondary descent -
 * the lock is taken with IRQs masked, so a non-fatal print from an ISR
 * could deadlock against the context it interrupted). Lines may interleave
 * mid-line with locked output. */
void board_early_print_raw(const char *message);

/* Render the current "[   s.mmm] " stamp into buf (16 bytes capacity);
 * returns its length. First call captures the boot epoch. */
int board_uptime_stamp(char buf[16]);

/* Boot-relative uptime as numbers, for callers that cross-check this clock
 * against the OS tick (the shell's uptime command). Either pointer may be
 * NULL. */
void board_uptime_parts(uint32_t *sec, uint32_t *ms);

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

/* Interrupt-path snapshot on the calling core: *pmr = ICC_PMR_EL1,
 * *rpr = ICC_RPR_EL1. Either pointer may be NULL. Read-only; see gicv3.c
 * for why RPR must never be confused with IAR0. */
void board_gicv3_diag(uint32_t *pmr, uint32_t *rpr);

/* --- tick ----------------------------------------------------------------- */

/* The tick INTID lives in board_conf.h (BOARD_TICK_INTID) and is selected
 * by core count: CNTV/INTID 27 single-core (the M0..M4 baseline),
 * CNTPNS/INTID 30 under SMP - the virtual timer's line pends but is never
 * delivered to the boot core in SMP mode on this board, while CNTPNS/PPI30
 * delivers (and has run for weeks as the RTEMS BSP's tick). The full story
 * is in that board's conf. */

/* True once the tick timer is armed and running. */
bool board_tick_is_running(void);

/* --- console -------------------------------------------------------------- */

/* The console UART's GIC INTID is BOARD_CONSOLE_INTID (board_conf.h). */

/* --- SMP ------------------------------------------------------------------- */

/* The number of cores, the redistributor frame stride, how a core's logical
 * number is extracted from MPIDR, and the PSCI conventions (conduit
 * instruction, CPU_ON target encoding) are board_conf.h policy. The
 * declarations below are the board-independent SMP surface every board
 * provides. */

/* The core count (BOARD_SMP_CORES) is board_conf.h policy, derived from the
 * SMP_CORES make flag so `make SMP_CORES=1` really builds a single-core
 * image everywhere (kernel configNUMBER_OF_CORES, port arrays, app fan-out
 * and the tick selection all derive from the same number). The declarations
 * below are the board-independent SMP surface every board provides. */

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
 * Call site follows the reference SMP line (D33): the kernel port's
 * xPortStartScheduler invokes StartSecondaryCpuUp() on core 0 BEFORE the
 * tick is armed, so every core is up and parked in kernel_secondary_main
 * waiting for the scheduler before the first task ever runs. (An earlier
 * arrangement released secondaries from a post-scheduler task; that was a
 * workaround from the rounds where the port itself was broken, and is gone
 * with it.) */
void board_smp_start_secondaries(void);

/* A secondary core calls this once its redistributor / CPU interface / SGI
 * setup is done. The boot core's bounded wait in
 * board_smp_start_secondaries() is waiting for exactly these reports. */
void board_smp_mark_core_up(uint32_t core);

/* Secondary descent stage marker, called from smp_secondary.S. Stamped raw
 * output - see smp.c for why it must not take the print lock. */
void board_smp_descent_marker(void);

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
 * The three named levels are the reference SMP line's board-validated tick
 * and SGI values (13 / 11), adopted with the port transplant (D33). The
 * FromISR ceiling is 14, NOT the reference line's 15 - and this is not a
 * stylistic choice:
 *  - 15 (0xf0) is UNSIGNABLE on this GIC: only the top 4 priority bits are
 *    implemented, ICC_PMR's implemented value therefore maxes out at 0xf,
 *    and a GIC signals interrupts strictly numerically BELOW the PMR - a
 *    priority-15 line can never beat the mask. (This is the same fact the
 *    classic single-core port encoded as "tick at lowest USABLE, not 15".)
 *    The reference line parks nothing at 15 - its device interrupts all sit
 *    at 10-13 - so its API=15 costs it nothing; every one of OUR FromISR
 *    drivers sits AT the API ceiling, and a board boot with API=15 left the
 *    tick (0xd0) delivering while the console RX, GMAC, the soft-trigger
 *    probe and every ITS LPI (all 0xf0) were silently starved.
 *  - 14 (0xe0) is the highest signable level, so it is what FromISR callers
 *    use; vPortValidateInterruptPriority asserts RPR >= 14 << 4 and every
 *    driver configured with *_RAW below satisfies it exactly.
 *  - TICK (13) is the reference line's tick level; the transplanted tick
 *    handler carries no RPR assertion.
 *  - SGI (11) arms SGI0 for the cross-core yield.
 * Getting any of these wrong is silent at build time and a starvation at
 * run time, which is why they are named here rather than written as
 * literals at call sites.
 *
 * 2026-09-18 board rounds: the SDK values (tick 13 / SGI 11) combined with
 * a FromISR ceiling of 14 reproduced a fatal board reset at the second GMAC's
 * PHY bring-up on BOTH the 2-core and the 1-core image, where the M-line's
 * 14 / 11 / 9 scheme never did. The scheme below is the M-line-accepted
 * values (tick at the lowest usable level 14 - on this GIC 15 is unsignable,
 * see the four-bit PMR arithmetic above - FromISR ceiling at 11, yield SGI
 * above the ceiling at 9); re-derive any move away from it on the board,
 * one variable at a time, before adopting it. */
#define BOARD_IRQ_PRIORITY_SHIFT	4U
#define BOARD_IRQ_PRIORITY_TICK		14U	/* lowest usable, not 15 */
#define BOARD_IRQ_PRIORITY_API_CALL	11U
#define BOARD_IRQ_PRIORITY_DEFAULT	10U

/* The kernel-test image's IntQueue stress source (a software-pended SPI,
 * board_conf.h BOARD_KTEST_INTQ_INTID). Sits between the FromISR ceiling
 * and the tick on purpose: numerically BELOW the tick level (12 < 14) so it
 * genuinely preempts the tick handler - the interrupt nesting the IntQueue
 * test exists to exercise - while staying numerically ABOVE the API-call
 * ceiling (12 > 11), which is what makes every FromISR call it performs
 * legal under vPortValidateInterruptPriority. */
#define BOARD_IRQ_PRIORITY_INTQ_TIMER	12U

/* The cross-core yield SGI (see BOARD_SMP_YIELD_INTID). Above API_CALL
 * (numerically lower) on purpose: the SGI must stay deliverable while a
 * core sits in a kernel critical section with ICC_PMR narrowed to
 * API_CALL, or cross-core preemption stalls for the whole critical
 * section. Its handler only sets a per-core "yield requested" flag and
 * never calls a kernel API, so running above the API-call level breaks no
 * port assertion. */
#define BOARD_IRQ_PRIORITY_SGI		9U

#define BOARD_IRQ_PRIORITY_TICK_RAW \
	(BOARD_IRQ_PRIORITY_TICK << BOARD_IRQ_PRIORITY_SHIFT)
#define BOARD_IRQ_PRIORITY_API_CALL_RAW \
	(BOARD_IRQ_PRIORITY_API_CALL << BOARD_IRQ_PRIORITY_SHIFT)
#define BOARD_IRQ_PRIORITY_DEFAULT_RAW \
	(BOARD_IRQ_PRIORITY_DEFAULT << BOARD_IRQ_PRIORITY_SHIFT)
#define BOARD_IRQ_PRIORITY_SGI_RAW \
	(BOARD_IRQ_PRIORITY_SGI << BOARD_IRQ_PRIORITY_SHIFT)
#define BOARD_IRQ_PRIORITY_INTQ_TIMER_RAW \
	(BOARD_IRQ_PRIORITY_INTQ_TIMER << BOARD_IRQ_PRIORITY_SHIFT)

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_BOARD_H */
