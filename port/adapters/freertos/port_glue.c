/*
 * @file   port_glue.c
 * @brief  The board-facing side of the FreeRTOS adapter.
 *
 * This is the ONLY place where the kernel and the board meet. Everything else
 * in the project talks to CMSIS-RTOS2 (include/cmsis_os2.h), so replacing the
 * kernel means rewriting this directory and port/adapters/cmsis_rtos2/ only.
 *
 * Shape transplanted from the board-validated reference SMP line's
 * freertos_configs.c (D33, see IMPORT-INFO.md); the standalone-SDK calls it
 * made map onto this tree's board layer as:
 *
 *   vConfigureTickInterrupt / vClearTickInterrupt
 *       -> the CMSIS OS_Tick_* interface (tick.c), same CNTPNS/PPI30 TVAL
 *          programming, single tick on core 0;
 *   StartSecondaryCpuUp -> board_smp_start_secondaries() (PSCI CPU_ON plus
 *       the per-core report-in flags, smp.c);
 *   InterruptSecondaryInit + SecondaryCoreStartup -> kernel_secondary_main()
 *       (per-core GIC bring-up, report in, wait for the scheduler, enter it);
 *   FExceptionInterruptHandler -> board_gicv3_dispatch();
 *   DbgRawPrint -> board_early_print (polled UART, spinlock, DAIF masked).
 *
 * Called out by the port proper:
 *   1. vApplicationInterruptHandler() - the port's IRQ entry has already
 *      read ICC_IAR1_EL1 and will write ICC_EOIR1_EL1 itself, so this only
 *      routes: tick, spurious, or the board's handler table;
 *   2. vApplicationInIrq() - in-interrupt predicate for xPortIsInsideInterrupt;
 *   3. the application hooks the config enables (malloc/stack/assert);
 *   4. the synchronous-exception / SError parking spots the port's vector
 *      table falls into for anything that is not a yield.
 */

#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "irq_ctrl.h"
#include "os_tick.h"

/* --- interrupt dispatch --------------------------------------------------- */

static volatile uint32_t is_in_irq = 0;

/* Called by the port's FreeRTOS_IRQ_Handler with the raw ICC_IAR1_EL1 value.
 * The entry code performs the EOI itself on the way out, so nothing here may
 * acknowledge or deactivate. */
void vApplicationInterruptHandler(uint32_t ulICCIAR)
{
	is_in_irq++;

	if (ulICCIAR < 8192)
	{
		/* Interrupts cannot be re-enabled until the source of the interrupt is
		 * cleared. The ID of the interrupt is obtained by bitwise ANDing the
		 * ICCIAR value with 0x3FF. */
		ulICCIAR = ulICCIAR & 0x3FFUL;
	}

	/* call handler function */
	if (ulICCIAR == (uint32_t)BOARD_TICK_INTID)
	{
		/* Generic Timer - the tick lives on core 0 only (single-tick
		 * architecture; the tick handler indexes ullPortYieldRequired[0]). */
		FreeRTOS_Tick_Handler();
	}
	else
	{
		if (ulICCIAR != 1023U)
		{
			/* Everything else - SPIs, the console, GMAC, and LPIs routed
			 * through the ITS - goes through the board's handler table. */
			board_gicv3_dispatch(ulICCIAR);
		}
		else
		{
			/* spurious（IAR=1023）是真异常信号；ISR 上下文走无锁
			 * raw 汇点（带戳，但绝不自旋在打印锁上） */
			board_early_print_raw("irq: spurious\n");
		}
	}
	is_in_irq--;
}

int vApplicationInIrq(void)
{
	return (int)is_in_irq;
}

/* --- tick ----------------------------------------------------------------- */

/* Arms the tick. Called by xPortStartScheduler via configSETUP_TICK_INTERRUPT,
 * on core 0 only.
 *
 * The timer is board_conf.h policy, selected by core count: CNTV on INTID 27
 * single-core, CNTPNS on INTID 30 under SMP (the virtual timer's line pends
 * but is never delivered to the boot core in SMP mode on this board - the
 * story is in board_conf.h). OS_Tick_Setup rearms through the CMSIS
 * OS_Tick_* interface, so this file never touches timer registers itself. */
void board_tick_port_setup(void)
{
	(void)OS_Tick_Setup(configTICK_RATE_HZ, FreeRTOS_Tick_Handler);
	OS_Tick_Enable();

	/* Make the selection and the arm visible. If the tick later proves
	 * dead, this line is the difference between "the wrong source was
	 * selected" and "the right source was armed but never delivered". */
	board_log("tick: INTID=%u %s armed, load=%u",
		  (unsigned)BOARD_TICK_INTID,
#if BOARD_SMP_CORES > 1
		  "cntpns"
#else
		  "cntv"
#endif
		  , (unsigned)OS_Tick_GetInterval());
}

/* --- SMP: secondary bring-up and the scheduler handshake ------------------- */

#if ( configNUMBER_OF_CORES > 1 )

/* Defined by the port proper (port.c): raised by core 0 right after the tick
 * is armed, which is the signal for the parked secondaries to enter. */
extern volatile uint64_t uxPortSchedularRunning;

/* Called by xPortStartScheduler on core 0, BEFORE the tick is armed - the
 * reference line's order, kept exactly: all secondaries are released and
 * have run their own GIC bring-up (and are parked in kernel_secondary_main
 * below, waiting) by the time the first task ever runs. */
void StartSecondaryCpuUp(void)
{
	board_smp_start_secondaries();
}

/* The kernel's cross-core yield (portYIELD_CORE): raise SGI
 * ulInterruptID on exactly the target core. The encoding (Aff1 in
 * ICC_SGI1R_EL1 bits [23:16], TargetList = 1, one write per core - the
 * silently-dropped-affinity lesson is in board_gicv3_send_sgi) lives in the
 * board layer. */
void vInterruptCore(uint32_t ulInterruptID, uint32_t ulCoreID)
{
	configASSERT(ulCoreID < (uint32_t)configNUMBER_OF_CORES);
	configASSERT(ulInterruptID < 16U);

	board_gicv3_send_sgi(ulInterruptID, 1UL << ulCoreID);
}

/* Secondary-core landing from smp_secondary.S (the board layer's neutral
 * hand-off symbol). Does not return.
 *
 * Order is the reference line's: (1) this core's own redistributor and CPU
 * interface, (2) report in - the boot core's bounded wait in
 * board_smp_start_secondaries is waiting for exactly this, (3) spin until
 * core 0 has armed the tick and raised uxPortSchedularRunning, (4) enter the
 * scheduler, which installs this core's VBAR and restores the first task
 * with interrupts enabled via the task's initial PSTATE. */
void kernel_secondary_main(void)
{
	uint32_t cpu_id = board_smp_core_id();

	configASSERT(cpu_id < (uint32_t)configNUMBER_OF_CORES);

	board_gicv3_secondary_init();

	board_smp_mark_core_up(cpu_id);

	while (uxPortSchedularRunning == 0)
	{
		;
	}

	(void)xPortStartScheduler();

	/* Not reached: the scheduler ends in vPortRestoreTaskContext. */
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

#else /* configNUMBER_OF_CORES == 1 */

/* smp_secondary.S branches here unconditionally, but a single-core image
 * never releases a secondary (StartSecondaryCpuUp does not exist below
 * configNUMBER_OF_CORES > 1), so this is a never-reached park. */
void kernel_secondary_main(void)
{
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

#endif /* configNUMBER_OF_CORES > 1 */

/* --- fault parking (reached from the port's vector table) ------------------ */

/* A synchronous exception that is not a yield (ESR EC != 0x15) lands here
 * from vSynchronousInterruptHandler / vSynchronousInterruptHandlerSPx. There
 * is no recovery path - report the fact and park, with the port's saved
 * frame still on this stack (it no longer matters). */
void SynchronousInterrupt(void *frame)
{
	(void)frame;
	char buf[64];
	unsigned long esr = 0UL, far = 0UL;

	board_early_print("fatal: synchronous exception\n");
	__asm__ __volatile__("mrs %0, esr_el1" : "=r"(esr));
	__asm__ __volatile__("mrs %0, far_el1" : "=r"(far));
	(void)snprintf(buf, sizeof(buf),
		       "fatal: ESR_EL1=%08lx FAR_EL1=%08lx\n",
		       esr, far & 0xffffffffUL);
	board_early_print(buf);
	taskDISABLE_INTERRUPTS();
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

/* Asynchronous external abort, from vSErrorInterruptHandler. */
void SErrorInterrupt(void *frame)
{
	(void)frame;
	board_early_print("fatal: SError (bus error)\n");
	taskDISABLE_INTERRUPTS();
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

/* --- application hooks the config enables --------------------------------- */

void vApplicationMallocFailedHook(void)
{
	/* Out of heap is fatal: the system cannot recover and continuing would
	 * fail in ways that look unrelated to the real cause. */
	board_early_print("fatal: FreeRTOS heap exhausted\n");
	taskDISABLE_INTERRUPTS();
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	(void)pcTaskName;
	board_early_print("fatal: task stack overflow\n");
	taskDISABLE_INTERRUPTS();
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

void board_assert_failed(const char *file, int line)
{
	(void)file;
	(void)line;
	/* Reporting the file and line would need a formatter; the console has
	 * only raw string output at this point. The distinct message is enough
	 * to tell an assertion from a hang when reading the serial log. */
	board_early_print("fatal: configASSERT failed\n");
	taskDISABLE_INTERRUPTS();
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}
