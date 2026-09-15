/*
 * @file   port_glue.c
 * @brief  The board-facing side of the FreeRTOS adapter.
 *
 * This is the ONLY place where the kernel and the board meet. Everything else
 * in the project talks to CMSIS-RTOS2 (include/cmsis_os2.h), so replacing the
 * kernel means rewriting this directory and port/adapters/cmsis_rtos2/ only.
 *
 * It supplies the three things the upstream ARM_AARCH64_SRE port expects the
 * integrator to provide:
 *
 *   1. configSETUP_TICK_INTERRUPT()  -> board_tick_port_setup()
 *   2. vApplicationIRQHandler()      -> the port has already read ICC_IAR1_EL1
 *                                      and will perform EOI itself, so this
 *                                      only routes the INTID to a handler
 *   3. the application hooks the config enables (malloc/stack/assert)
 *
 * The tick is armed through the CMSIS OS_Tick_* interface rather than by
 * poking CNTV here, so the board layer keeps no kernel dependency and the same
 * tick code serves any kernel.
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "irq_ctrl.h"
#include "os_tick.h"

/* --- tick ----------------------------------------------------------------- */

void board_tick_port_setup(void);

/* Arms the tick. Called by xPortStartScheduler via configSETUP_TICK_INTERRUPT.
 *
 * The timer is CNTV on INTID 27, for reasons documented in board.h: the
 * physical timer is claimed by OP-TEE and the EL2 physical timer is gated by
 * CNTHCTL_EL2.EL1PCEN which the `go` boot path leaves clear.
 *
 * The handler passed to OS_Tick_Setup is the kernel's own tick entry, which is
 * what re-arms the timer and performs the scheduler bookkeeping. */
void board_tick_port_setup(void)
{
	(void)OS_Tick_Setup(configTICK_RATE_HZ, FreeRTOS_Tick_Handler);
	OS_Tick_Enable();
}

/* --- interrupt dispatch ---------------------------------------------------- */

/* The port's FreeRTOS_IRQ_Handler has already read ICC_IAR1_EL1 into
 * ulICCAck, incremented the nesting count, and will write ICC_EOIR1_EL1 on the
 * way out. So this function must NOT acknowledge or EOI - doing either again
 * would corrupt the controller state.
 *
 * Spurious INTID 1023 is dropped here rather than dispatched. */
void vApplicationIRQHandler(uint32_t ulICCAck)
{
	board_gicv3_dispatch(ulICCAck);
}

/* --- application hooks the config enables --------------------------------- */

void vApplicationMallocFailedHook(void)
{
	/* Out of heap is fatal: the system cannot recover and continuing would
	 * fail in ways that look unrelated to the real cause. */
	board_early_print("\n[fatal] FreeRTOS heap exhausted\n");
	taskDISABLE_INTERRUPTS();
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	(void)pcTaskName;
	board_early_print("\n[fatal] task stack overflow\n");
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
	board_early_print("\n[fatal] configASSERT failed\n");
	taskDISABLE_INTERRUPTS();
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}
