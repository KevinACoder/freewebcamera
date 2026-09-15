/*
 * @file   tick.c
 * @brief  RTOS tick from the EL1 virtual timer (CNTV), INTID 27.
 *
 * Exposed through the CMSIS OS_Tick_* interface shape rather than as a raw
 * hook, so the board layer carries no dependency on kernel internals:
 * swapping FreeRTOS out must not require editing this file.
 *
 * ---------------------------------------------------------------------------
 * Why the virtual timer, and not the physical one.
 *
 * This is the single most confusing thing about timers on this board, and the
 * obvious choice is the wrong one:
 *
 *  - The EL1 *physical* timer is unusable. Firmware routes it as a Group 0
 *    interrupt (OP-TEE), and non-secure code cannot enable it: writes to the
 *    GICR enable bit for PPI 29 are ignored. It also requires
 *    CNTHCTL_EL2.EL1PCEN, and the U-Boot `go` path leaves that bit clear.
 *
 *  - The EL2 physical timer (PPI 26) is visible to non-secure code but
 *    equally unusable, for the same CNTHCTL_EL2.EL1PCEN reason.
 *
 *  - The EL1 *virtual* timer (CNTV, PPI 27) works. Measured on this board:
 *    writing CNTV_TVAL raises GICR_ISPENDR0 bit 27, and a write to
 *    GICR_ISENABLER0 bit 27 takes effect.
 *
 * So the tick is CNTV on INTID 27. Note this is deliberately NOT the
 * architectural PPI 14 / INTID 30 that a generic Cortex-A port would assume.
 *
 * The kernel port requires the tick to run at the LOWEST USABLE interrupt
 * priority, which is one level above the absolute lowest (the port reserves
 * that for itself): FreeRTOS_Tick_Handler asserts that the running priority
 * equals portLOWEST_USABLE_INTERRUPT_PRIORITY. Anything that calls a FromISR
 * API from an interrupt must instead be configured at
 * configMAX_API_CALL_INTERRUPT_PRIORITY, or the port's
 * vPortValidateInterruptPriority assertion fires. Both raw values come from
 * board.h so there is one place to get them wrong.
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>

#include "board.h"
#include "regs.h"
#include "irq_ctrl.h"
#include "os_tick.h"

/* Tick period in microseconds, and the derived reload value. */
static uint32_t tick_freq_hz;
static uint32_t tick_load;
static uint32_t tick_irq = BOARD_TICK_INTID;
static volatile uint32_t tick_overflow_count;
static volatile bool tick_enabled;

/* Priority assigned to the tick: the lowest the kernel tolerates, taken from
 * the board's single priority policy (board.h) so this driver and the kernel
 * port cannot disagree.
 *
 * IRQ_SetPriority (CMSIS irq_ctrl.h) takes the raw 8-bit GIC value, not the
 * logical level, so the *_RAW form is what goes to the register. The port's
 * FreeRTOS_Tick_Handler asserts ICC_RPR_EL1 equals
 * portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT; with 16 unique
 * priorities that is 14 << 4 = 0xe0. A raw 0xf0 (logical 15) is a different
 * hardware level and trips the assertion. */
#define BOARD_TICK_PRIORITY	BOARD_IRQ_PRIORITY_TICK_RAW

static inline uint64_t read_cntfrq(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(value));
	__asm__ __volatile__("isb" ::: "memory");
	return value;
}

static inline void write_cntv_tval(uint64_t value)
{
	__asm__ __volatile__("msr cntv_tval_el0, %0" ::"r"(value));
	__asm__ __volatile__("isb" ::: "memory");
}

static inline uint64_t read_cntv_tval(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntv_tval_el0" : "=r"(value));
	return value;
}

static inline uint64_t read_cntv_ctl(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntv_ctl_el0" : "=r"(value));
	return value;
}

static inline void write_cntv_ctl(uint64_t value)
{
	__asm__ __volatile__("msr cntv_ctl_el0, %0" ::"r"(value));
	__asm__ __volatile__("isb" ::: "memory");
}

/* The reload value for one tick, in counter units. */
static uint32_t compute_load(void)
{
	uint64_t freq = read_cntfrq();

	if (freq == 0u || tick_freq_hz == 0u) {
		return 0u;
	}
	return (uint32_t)(freq / tick_freq_hz);
}

int32_t OS_Tick_Setup(uint32_t freq, IRQHandler_t handler)
{
	if (freq == 0u) {
		return -1;
	}

	tick_freq_hz = freq;
	tick_load = compute_load();
	if (tick_load == 0u) {
		return -1;
	}

	IRQ_Initialize();

	/* Stop the timer while it is reprogrammed, then set the period. The
	 * control register's enable bit is bit 0 and the interrupt mask is
	 * bit 1; writing 0 masks the interrupt and stops the timer, which is
	 * the only safe state to program from. */
	write_cntv_ctl(0);
	write_cntv_tval((uint64_t)tick_load);

	IRQ_Disable((IRQn_ID_t)tick_irq);
	IRQ_ClearPending((IRQn_ID_t)tick_irq);
	IRQ_SetHandler((IRQn_ID_t)tick_irq, handler);
	/* The timer's line is level-sensitive: it stays asserted while the
	 * condition holds and drops when CNTV_TVAL is reloaded. Requesting edge
	 * semantics here would be wrong even though the level/edge selection
	 * for PPIs is not written to ICFGR yet. */
	IRQ_SetMode((IRQn_ID_t)tick_irq, IRQ_MODE_TRIG_LEVEL |
					   IRQ_MODE_TYPE_IRQ |
					   IRQ_MODE_DOMAIN_NONSECURE);
	/* Raw hardware value, not the logical level: see the note above. */
	IRQ_SetPriority((IRQn_ID_t)tick_irq, BOARD_TICK_PRIORITY);
	IRQ_Enable((IRQn_ID_t)tick_irq);

	/* Enable with the interrupt unmasked (bit 1 clear, bit 0 set). */
	write_cntv_ctl(0x1);
	tick_enabled = true;
	tick_overflow_count = 0u;

	return 0;
}

void OS_Tick_Enable(void)
{
	if (!tick_enabled) {
		return;
	}
	write_cntv_tval((uint64_t)tick_load);
	write_cntv_ctl(0x1);
}

void OS_Tick_Disable(void)
{
	tick_enabled = false;
	write_cntv_ctl(0x0);
}

void OS_Tick_AcknowledgeIRQ(void)
{
	/* The interrupt is level-triggered and clears when the timer is
	 * rearmed. The kernel port performs the EOI itself (it reads
	 * ICC_IAR1_EL1 and writes ICC_EOIR1_EL1 around the dispatch), so this
	 * only reloads the period. */
	write_cntv_tval((uint64_t)tick_load);
}

int32_t OS_Tick_GetIRQn(void)
{
	return (int32_t)tick_irq;
}

uint32_t OS_Tick_GetClock(void)
{
	return (uint32_t)read_cntfrq();
}

uint32_t OS_Tick_GetInterval(void)
{
	return tick_load;
}

uint32_t OS_Tick_GetCount(void)
{
	/* Time left in the current period, as a count-down value. The kernel
	 * uses this to size a sleep without waiting a whole tick. */
	return (uint32_t)read_cntv_tval();
}

uint32_t OS_Tick_GetOverflow(void)
{
	uint32_t count = tick_overflow_count;

	/* Read-and-clear: the caller wants the overflows since the last call,
	 * not a running total. */
	tick_overflow_count = 0u;
	return count;
}

/* Called from the tick handler when a period elapses without being
 * acknowledged, so a stalled scheduler surfaces as a count instead of a
 * silently drifting clock. */
void board_tick_note_overflow(void)
{
	tick_overflow_count++;
}

/* --- kernel port hooks ---------------------------------------------------- */

/* The kernel port drives the tick through configSETUP_TICK_INTERRUPT, which
 * calls OS_Tick_Setup with its configured rate. This predicate is what the
 * boot path uses to report whether the tick source came up - part of the M0
 * acceptance evidence. */
bool board_tick_is_running(void)
{
	return tick_enabled && ((read_cntv_ctl() & 0x1u) != 0u);
}
