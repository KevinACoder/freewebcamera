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
 * The kernel port requires the tick to run at the LOWEST usable interrupt
 * priority: FreeRTOS_Tick_Handler asserts that the running priority equals
 * portLOWEST_USABLE_INTERRUPT_PRIORITY. Anything that calls a FromISR API
 * from an interrupt must instead be configured at
 * configMAX_API_CALL_INTERRUPT_PRIORITY, or the port's
 * vPortValidateInterruptPriority assertion fires.
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

/* Priority assigned to the tick: the lowest the kernel tolerates. Kept in one
 * place so the kernel port and this driver cannot disagree. */
#ifndef BOARD_TICK_PRIORITY
#define BOARD_TICK_PRIORITY	0xf0u
#endif

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
	IRQ_SetMode((IRQn_ID_t)tick_irq, IRQ_MODE_TRIG_EDGE_RISING |
					   IRQ_MODE_TYPE_IRQ |
					   IRQ_MODE_DOMAIN_NONSECURE);
	/* Lowest usable priority: FreeRTOS_Tick_Handler asserts this. */
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
