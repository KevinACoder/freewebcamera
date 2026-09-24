/*
 * @file   main.c
 * @brief  trunk baseline boot: ThreadX UP + gdb stub + cherrysh, through
 *         CMSIS-RTOS2 only.
 *
 * Boot anchors, in order (board acceptance for this baseline):
 *   banner + cntfrq      console alive, CNTFRQ readable from EL1
 *   its: LPI OK          the ITS/LPI path delivers (also the MSI substrate
 *                        the future USB/xHCI line will sit on)
 *   app: SPI SOFTTRIG OK a software-pended SPI reached its handler (GIC)
 *   shell: READY         cherrysh task runs, console RX armed, prompt up
 *
 * The gdb stub is present from boot (tx_gdb_init() runs in the ThreadX
 * port's kernel entry) but never owns the console unless a session starts:
 * the shell owns RX, and a 0x03 byte on the console (or a tick-poll Ctrl-C
 * once the stub has claimed the line) breaks into the stub. See
 * tx_gdb_glue.c for the arbitration.
 */

#include <stdint.h>

#include "Driver_USART.h"
#include "board.h"
#include "gicv3_its.h"
#include "cmsis_os2.h"
#include "irq_ctrl.h"
#include "shell.h"

#include "dbg_scenario.h"

/* Image identity on the console and in the TFTP staging log. */
#define IMAGE_BANNER	"\nfreewebcamera trunk - RK3568 ThreadX UP + gdb stub + shell\n"

/* Console handle. Taken from the interface, never from the driver's header. */
extern ARM_DRIVER_USART Driver_USART_Console;

/* Console-interrupt path probe (drivers/uart_ns16550.c): one stamped line
 * with the INTID 150 pending/active state. Kept from the bring-up rounds:
 * it brackets console-interrupt state around kernel start at zero cost. */
extern void uart_console_line_probe(const char *tag);

/* Spare SPI line used as the software-trigger probe. Chosen in the SPI range
 * and not otherwise routed to anything on this board. */
#define SPI_PROBE_INTID		60U
/* IRQ_SetPriority takes the raw GIC byte, so the board's logical level is
 * shifted per the board's own policy rather than shifted here. */
#define SPI_PROBE_PRIORITY	BOARD_IRQ_PRIORITY_API_CALL_RAW

static ARM_USART_SignalEvent_t console_event;
static volatile uint32_t spi_hits;

/* --- SPI soft-trigger probe ---------------------------------------------- */

static void spi_probe_handler(void)
{
	IRQ_ClearPending((IRQn_ID_t)SPI_PROBE_INTID);
	spi_hits++;
}

/* --- shell bring-up task --------------------------------------------------- */

/* A task, not a call from board_main: shell_start() arms the console receive
 * interrupt, whose path ends in osThreadFlagsSetFromISR - which needs a
 * running scheduler. Called from board_main before osKernelStart() it would
 * arm that interrupt with nothing behind it.
 *
 * The boot anchors run here too, ahead of the shell: its_selftest() takes
 * milliseconds, and none of that belongs between the banner and the first
 * log line a person can interrupt. */
static void task_shell_start(void *argument)
{
	uint32_t delivered = 0U;

	(void)argument;

	if (its_selftest(&delivered) != 0) {
		board_log("its: LPI FAIL\n");
	} else {
		board_log("its: LPI OK (n=%u)\n", (unsigned)delivered);
	}

	if (spi_hits == 0U) {
		board_log("app: SPI SOFTTRIG FAIL (no handler entry)\n");
	} else {
		board_log("app: SPI SOFTTRIG OK\n");
	}

	if (shell_start() != 0) {
		board_log("shell: FAIL\n");
		return;
	}
	board_log("shell: READY\n");

	/* Nothing left to do: the shell owns its own task from here. Terminating
	 * rather than idling keeps the stack and the slot free. */
	osThreadTerminate(osThreadGetId());
}

/* --- boot ----------------------------------------------------------------- */

void board_main(void)
{
	uint32_t cntfrq;

	(void)Driver_USART_Console.Initialize(console_event);
	(void)Driver_USART_Console.PowerControl(ARM_POWER_FULL);

	board_early_print(IMAGE_BANNER);

	/* Report the counter frequency: proves CNTV/CNTFRQ are reachable from
	 * EL1, which is the precondition for the tick working at all. */
	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(cntfrq));
	board_log("board: cntfrq=%u\n", (unsigned)cntfrq);

	/* Interrupt controller, through the CMSIS interface. */
	(void)IRQ_Initialize();
	uart_console_line_probe("post-irqinit");

	if (IRQ_SetHandler((IRQn_ID_t)SPI_PROBE_INTID, spi_probe_handler) != 0) {
		board_log("app: SPI handler install FAIL\n");
	}
	(void)IRQ_SetPriority((IRQn_ID_t)SPI_PROBE_INTID, SPI_PROBE_PRIORITY);
	(void)IRQ_Enable((IRQn_ID_t)SPI_PROBE_INTID);

	/* Kernel init through CMSIS-RTOS2. */
	if (osKernelInitialize() != osOK) {
		board_log("fatal: osKernelInitialize failed\n");
		return;
	}

	if (osThreadNew(task_shell_start, 0, &(osThreadAttr_t){ .name = "shstart",
			.stack_size = 2048, .priority = osPriorityHigh }) == 0) {
		board_log("fatal: thread shell\n");
		return;
	}

	/* Pend the probe SPI. It stays pending until interrupts are unmasked,
	 * so the first handler entry proves the whole path: distributor enable,
	 * CPU interface, priority, and dispatch. */
	(void)IRQ_SetPending((IRQn_ID_t)SPI_PROBE_INTID);

	uart_console_line_probe("pre-kstart");

	/* Never returns. */
	(void)osKernelStart();

	board_log("fatal: kernel returned\n");
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}
