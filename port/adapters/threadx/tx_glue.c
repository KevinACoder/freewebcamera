/*
 * @file   tx_glue.c
 * @brief  The board-facing side of the ThreadX adapter.
 *
 * This file plays the exact role port_glue.c plays for the FreeRTOS image:
 * the ONE place where the ThreadX kernel meets the board layer. Everything
 * else in the project talks to CMSIS-RTOS2 (include/cmsis_os2.h), so
 * swapping kernels touches this directory, tx_vectors.S and
 * port/adapters/cmsis_rtos2_threadx/ only.
 *
 * What the port expects the BSP to provide, and where it comes from here:
 *   ICC_IAR1_EL1 read / ICC_EOIR1_EL1 write
 *       -> tx_irq_handler() below (encodings match portasm_smp.S, which the
 *          FreeRTOS image board-proved);
 *   a periodic tick interrupt that reaches _tx_timer_interrupt()
 *       -> the CMSIS OS_Tick_* interface (tick.c), same CNTPNS INTID 30
 *          single-tick model under SMP, armed in tx_application_define;
 *   cross-core preemption
 *       -> board_gicv3_send_sgi(). NOTE: the upstream port's
 *          tx_thread_smp_core_preempt.S encodes the target core in the
 *          ICC_SGI1R_EL1 Aff0 field (bits [23:16]) - and, without
 *          TX_ARMV8_2, as a TargetList shift. This board numbers its cores
 *          in MPIDR Aff1 (Aff0 is always 0), so either encoding targets NO
 *          core and the SGI is silently dropped - the same class of bug the
 *          FreeRTOS port hit (Aff1 must sit in bits [39:32]). The upstream
 *          .S is therefore NOT compiled for this image; the adapter provides
 *          the symbol via the board's proven SGI sender. Registered in
 *          IMPORT-INFO.md;
 *   secondaries parked until the kernel is up
 *       -> _tx_thread_smp_initialize_wait(), entered from
 *          kernel_secondary_main (the board's neutral hand-off symbol,
 *          landed via the SPSel stub in tx_vectors.S);
 *   __top_of_ram
 *       -> linker alias for __heap_end (rk3568.ld), feeding
 *          _tx_initialize_unused_memory.
 *
 * SPSel model: the port runs threads, kernel and ISRs all on SP_EL0 (the
 * initial thread frame carries SPSR M=0x4 = EL1t). tx_vectors.S switches
 * SPSel 1 -> 0 before entering the kernel on each core; from there nothing
 * returns past the switch.
 */

#include <stdint.h>

#include "tx_api.h"
#include "tx_thread.h"

#include "board.h"
#include "irq_ctrl.h"
#include "os_tick.h"

/* ThreadX tick rate, mirroring the FreeRTOS image's configTICK_RATE_HZ. */
#define TX_TICK_RATE_HZ		1000U

/* The preemption SGI, board policy: same INTID the FreeRTOS image uses
 * (portYIELD_CORE_INT_ID = 0), same raw priority, already exercised by the
 * board layer's GIC bring-up on every core. */
#define TX_PREEMPT_SGI_INTID	0U

/* The port's IRQ dispatcher function (tx_timer_interrupt.S). Not declared
 * by the common headers - it is port-owned - so declare it here. */
extern void _tx_timer_interrupt(void);

/* Provided by port/adapters/cmsis_rtos2_threadx/: creates the threads the
 * CMSIS layer deferred while the kernel was not yet running. */
extern void tx_cmsis_application_define(void *first_unused_memory);

/* --- GIC system-register interface ---------------------------------------- */

/* Same encodings portasm_smp.S board-proved: IAR1 = S3_0_C12_C12_0,
 * EOIR1 = S3_0_C12_C12_1. (IAR0, S3_0_C12_C8_0, is the GROUP 0 ack register
 * - reading it from NS EL1 traps to EL3 and OP-TEE resets the machine.) */
static inline uint32_t icc_iar1_read(void)
{
	uint32_t v;

	__asm__ __volatile__("mrs %0, s3_0_c12_c12_0" : "=r"(v));
	return v;
}

static inline void icc_eoir1_write(uint32_t v)
{
	__asm__ __volatile__("msr s3_0_c12_c12_1, %0" :: "r"(v));
}

static void vbar_install(void)
{
	__asm__ __volatile__(
		"ldr	x0, =_threadx_vector_table\n"
		"msr	vbar_el1, x0\n"
		"isb\n"
		:: : "x0");
}

/* --- interrupt dispatch ---------------------------------------------------- */

/* Called from tx_vectors.S with the frame already saved by
 * _tx_thread_context_save. This file acknowledges (EOI) - the port's entry
 * code does not touch the GIC. */
void tx_irq_handler(void)
{
	uint32_t raw = icc_iar1_read();
	uint32_t id = raw & 0x3ffU;

	if (id == (uint32_t)BOARD_TICK_INTID) {
		/* Tick: expirations + per-core time slice, under the kernel's
		 * own SMP protection inside _tx_timer_interrupt, then the
		 * rearm. The rearm (OS_Tick_AcknowledgeIRQ reloads TVAL) is
		 * what deasserts the level-triggered line - the first boot
		 * skipped it here and core 0 sat in a tick storm (the line
		 * re-pended the moment EOI landed) while the other cores
		 * carried the shell. The FreeRTOS equivalent is
		 * configCLEAR_TICK_INTERRUPT inside FreeRTOS_Tick_Handler. */
		_tx_timer_interrupt();
		OS_Tick_AcknowledgeIRQ();
	} else if (id == TX_PREEMPT_SGI_INTID) {
		/* Preempt IPI: deliberately empty. The IRQ exit path
		 * (_tx_thread_context_restore) re-reads _tx_thread_execute_ptr
		 * and performs the preemption itself; the IPI only has to
		 * interrupt the target core's masked poll loop. */
	} else if (id != 1023U) {
		/* Everything else - console, and any SPI/LPI the board routed
		 * - goes through the board's handler table. */
		board_gicv3_dispatch(id);
	} else {
		/* Spurious (IAR=1023): keep the bare-UART marker the FreeRTOS
		 * glue prints, so both kernels produce comparable logs. No EOI
		 * for a spurious ack. */
		board_early_print("  [irq] spurious\r\n");
	}

	if (id != 1023U) {
		icc_eoir1_write(raw);
	}
}

/* --- SMP: cross-core preemption -------------------------------------------- */

/* Replacement for the port's tx_thread_smp_core_preempt.S - see the file
 * comment for why the upstream encoding cannot reach this board's cores.
 * The Aff1-in-bits[39:32] encoding lives in the board layer. */
void _tx_thread_smp_core_preempt(UINT core)
{
	board_gicv3_send_sgi(TX_PREEMPT_SGI_INTID, 1UL << core);
}

/* The IPI's work happens in the entry/exit machinery (see tx_irq_handler);
 * the handler itself is a no-op. */
static void tx_preempt_isr(void)
{
}

/* Per-core registration of the preempt SGI. IRQ_Enable on an SGI is banked
 * at the redistributor, so EVERY core runs this: core 0 from
 * tx_application_define, secondaries from tx_secondary_main. */
static void tx_smp_ipi_setup(void)
{
	IRQ_SetHandler((IRQn_ID_t)TX_PREEMPT_SGI_INTID, tx_preempt_isr);
	IRQ_SetPriority((IRQn_ID_t)TX_PREEMPT_SGI_INTID,
			BOARD_IRQ_PRIORITY_SGI_RAW);
	IRQ_Enable((IRQn_ID_t)TX_PREEMPT_SGI_INTID);
}

/* --- tick ------------------------------------------------------------------ */

/* Registered with OS_Tick_Setup to satisfy its handler parameter, but
 * NEVER CALLED: the ThreadX dispatch path (tx_irq_handler) routes the tick
 * INTID directly - see the storm note there. The FreeRTOS image has the
 * same shape (its registered handler is also not who runs it). */
static void tx_tick_wrapper(void)
{
	_tx_timer_interrupt();
	OS_Tick_AcknowledgeIRQ();
}

/* Arms the tick on core 0, mirroring the FreeRTOS glue's
 * board_tick_port_setup: same OS_Tick_* path, same INTID selection policy
 * (board_conf.h picks CNTPNS INTID 30 under SMP). Runs from
 * tx_application_define, with CPU interrupts still masked - the timer pends
 * until _tx_thread_schedule unmasks them. */
static void tx_tick_setup(void)
{
	(void)OS_Tick_Setup(TX_TICK_RATE_HZ, tx_tick_wrapper);
	OS_Tick_Enable();

	board_log("tick: INTID=%u %s armed, load=%u",
		  (unsigned)BOARD_TICK_INTID,
#if BOARD_SMP_CORES > 1
		  "cntpns"
#else
		  "cntv"
#endif
		  , (unsigned)OS_Tick_GetInterval());
}

/* --- kernel entry points ---------------------------------------------------- */

/* ThreadX's tx_kernel_enter calls this: create the application objects, arm
 * the tick, install this core's runtime vector table. Runs on core 0 with
 * interrupts masked, on the boot stack. */
void tx_application_define(void *first_unused_memory)
{
	vbar_install();
	tx_tick_setup();
	tx_smp_ipi_setup();

	tx_cmsis_application_define(first_unused_memory);
}

/* Secondary landing, C part (after the SPSel stub in tx_vectors.S).
 * Order mirrors the FreeRTOS glue's kernel_secondary_main: own GIC
 * bring-up, own vector table, own SGI registration, report in, then park in
 * the port's initialization wait - which releases this core into
 * _tx_thread_schedule once core 0 finishes kernel init. Never returns. */
void tx_secondary_main(void)
{
	uint32_t cpu_id = board_smp_core_id();

	vbar_install();

	board_gicv3_secondary_init();

	tx_smp_ipi_setup();

	board_smp_mark_core_up(cpu_id);

	(void)_tx_thread_smp_initialize_wait();

	/* Not reached: the wait jumps straight into the scheduling loop. */
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

/* --- fault parking (reached from tx_vectors.S) ------------------------------ */

/* kind: 1 = current EL with SP0, 2 = any other entry. There is no recovery
 * path - report and park. */
void tx_fault_park(uint32_t kind)
{
	unsigned long esr = 0UL, far = 0UL;

	board_early_print("\n[threadx] FAULT: synchronous exception\n");
	__asm__ __volatile__("mrs %0, esr_el1" : "=r"(esr));
	__asm__ __volatile__("mrs %0, far_el1" : "=r"(far));
	{
		/* vsnprintf pulls in half of minilibc; the raw hex dump via
		 * polled puts keeps this path dependency-free, like the
		 * FreeRTOS glue's fault dumps but smaller. */
		char buf[64];

		(void)snprintf(buf, sizeof(buf),
			       "[fault] kind=%u ESR_EL1=%08lx FAR_EL1=%08lx\n",
			       (unsigned)kind, esr, far & 0xffffffffUL);
		board_early_print(buf);
	}

	for (;;) {
		__asm__ __volatile__("wfe");
	}
}
