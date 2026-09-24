/*
 * @file   tick.c
 * @brief  RTOS tick from an EL1 ARM generic timer, in the CMSIS OS_Tick_*
 *         interface shape (so the board layer carries no kernel dependency:
 *         swapping FreeRTOS out must not require editing this file).
 *
 * ---------------------------------------------------------------------------
 * Which timer, and why - this is the single most confusing thing about
 * timers on this board. There are TWO working answers, selected by core
 * count in board_conf.h (BOARD_TICK_INTID):
 *
 *  - SINGLE-CORE: the EL1 *virtual* timer (CNTV, INTID 27). Measured on
 *    this board: writing CNTV_TVAL raises GICR_ISPENDR0 bit 27 and INTID 27
 *    arrives. This is the M0..M4 acceptance baseline.
 *
 *  - SMP: the EL1 *non-secure physical* timer (CNTPNS, INTID 30). The
 *    virtual timer's line is enabled and pends correctly under SMP, but the
 *    GIC-600 never delivers it to the boot core - enabled+pending forever,
 *    reproducible, SMP-mode-specific (the reference SMP line spent six
 *    rounds excluding PMR, IGROUPR, WAKER and every other theory before
 *    isolating this). CNTPNS/PPI30 delivers; PPI 29 is a different line
 *    entirely - the SECURE physical timer (CNTPS) that OP-TEE holds as
 *    Group 0. An earlier comment here claimed the physical timer was
 *    unusable because the `go` path leaves CNTHCTL_EL2.EL1PCEN clear: that
 *    was falsified - startup.S's own EL2 descent sets EL1PCTEN|EL1PCEN
 *    before any EL1 code runs, and EL1 access to the non-secure physical
 *    timer registers is legal and board-proven.
 *
 * Either way the tick runs at the kernel tick priority (logical 13 with the
 * port's 16-level scheme, BOARD_IRQ_PRIORITY_TICK in board.h). Any interrupt
 * that calls a FromISR API from an interrupt must instead be configured at
 * configMAX_API_CALL_INTERRUPT_PRIORITY (the reference scheme's lowest
 * level), or the port's vPortValidateInterruptPriority assertion fires.
 * Both raw values come from board.h so there is one place to get them
 * wrong.
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

/* Priority assigned to the tick: BOARD_IRQ_PRIORITY_TICK (logical 14) from
 * the board's single priority policy (board.h), so this driver and the
 * kernel port cannot disagree.
 *
 * IRQ_SetPriority (CMSIS irq_ctrl.h) takes the raw 8-bit GIC value, not the
 * logical level, so the *_RAW form is what goes to the register: with 16
 * unique priorities that is 14 << 4 = 0xe0 - the LOWEST USABLE level (15 is
 * unsignable on this GIC's four-bit PMR comparison; see board.h). The
 * transplanted tick handler carries no RPR assertion, but the M-line
 * accepted this level and the SDK-value probe (tick 13) was implicated in a
 * board reset round - stay here unless the board says otherwise. */
#define BOARD_TICK_PRIORITY	BOARD_IRQ_PRIORITY_TICK_RAW

static inline uint64_t read_cntfrq(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(value));
	__asm__ __volatile__("isb" ::: "memory");
	return value;
}

/* The timer register accessors, selected by core count exactly as
 * BOARD_TICK_INTID was in board_conf.h: CNTV (virtual timer) single-core,
 * CNTPNS (non-secure physical timer) under SMP. Same programming model -
 * down-counter TVAL, control ENABLE bit 0 / IMASK bit 1, level interrupt
 * cleared by rewriting TVAL - only the register bank differs. */
#if BOARD_SMP_CORES > 1

static inline void tick_timer_write_tval(uint64_t value)
{
	__asm__ __volatile__("msr cntp_tval_el0, %0" ::"r"(value));
	__asm__ __volatile__("isb" ::: "memory");
}

static inline uint64_t tick_timer_read_tval(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntp_tval_el0" : "=r"(value));
	return value;
}

static inline uint64_t tick_timer_read_ctl(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntp_ctl_el0" : "=r"(value));
	return value;
}

static inline void tick_timer_write_ctl(uint64_t value)
{
	__asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(value));
	__asm__ __volatile__("isb" ::: "memory");
}

#else

static inline void tick_timer_write_tval(uint64_t value)
{
	__asm__ __volatile__("msr cntv_tval_el0, %0" ::"r"(value));
	__asm__ __volatile__("isb" ::: "memory");
}

static inline uint64_t tick_timer_read_tval(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntv_tval_el0" : "=r"(value));
	return value;
}

static inline uint64_t tick_timer_read_ctl(void)
{
	uint64_t value;

	__asm__ __volatile__("mrs %0, cntv_ctl_el0" : "=r"(value));
	return value;
}

static inline void tick_timer_write_ctl(uint64_t value)
{
	__asm__ __volatile__("msr cntv_ctl_el0, %0" ::"r"(value));
	__asm__ __volatile__("isb" ::: "memory");
}

#endif

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
	tick_timer_write_ctl(0);
	tick_timer_write_tval((uint64_t)tick_load);

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
	tick_timer_write_ctl(0x1);
	tick_enabled = true;
	tick_overflow_count = 0u;

	return 0;
}

void OS_Tick_Enable(void)
{
	if (!tick_enabled) {
		return;
	}
	tick_timer_write_tval((uint64_t)tick_load);
	tick_timer_write_ctl(0x1);
}

void OS_Tick_Disable(void)
{
	tick_enabled = false;
	tick_timer_write_ctl(0x0);
}

void OS_Tick_AcknowledgeIRQ(void)
{
	/* The interrupt is level-triggered and clears when the timer is
	 * rearmed. The kernel port performs the EOI itself (it reads
	 * ICC_IAR1_EL1 and writes ICC_EOIR1_EL1 around the dispatch), so this
	 * only reloads the period. */
	tick_timer_write_tval((uint64_t)tick_load);
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
	return (uint32_t)tick_timer_read_tval();
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
	return tick_enabled && ((tick_timer_read_ctl() & 0x1u) != 0u);
}
