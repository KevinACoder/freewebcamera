/*
 * @file   main.c
 * @brief  M0 acceptance: bring-up anchors, all driven through CMSIS-RTOS2.
 *
 * The point of this file is to prove the interfaces are real, not decorative:
 * every task, semaphore, queue and delay below goes through include/cmsis_os2.h
 * (`os*`), never through the FreeRTOS API. Nothing here includes FreeRTOS.h -
 * tools/check-deps.sh enforces that, and the kernel-replacement stub build
 * (K4) depends on it staying true.
 *
 * Anchors printed, in order:
 *   banner + cntfrq      console and CPU counter readable
 *   SWITCH OK            tasks actually switch (context switch works)
 *   TICK OK              the tick advances (CNTV/INTID 27 works)
 *   SPI SOFTTRIG OK      a software-pended SPI reaches its handler (GIC works)
 *
 * Why SPI is verified by behaviour and not by readback: the group status
 * registers read as zero on this silicon, so IRQ_GetEnableState cannot answer
 * from hardware. A handler counter is the only honest evidence.
 */

#include <stdint.h>

#include "Driver_USART.h"
#include "board.h"
#include "cmsis_os2.h"
#include "irq_ctrl.h"

/* Console handle. Taken from the interface, never from the driver's header. */
extern ARM_DRIVER_USART Driver_USART_Console;

/* Spare SPI line used as the software-trigger probe. Chosen in the SPI range
 * and not otherwise routed to anything on this board. */
#define SPI_PROBE_INTID		60U
/* IRQ_SetPriority takes the raw GIC byte, so the board's logical level is
 * shifted per the board's own policy rather than shifted here. */
#define SPI_PROBE_PRIORITY	BOARD_IRQ_PRIORITY_API_CALL_RAW

static ARM_USART_SignalEvent_t console_event;
static volatile uint32_t spi_hits;
static volatile uint32_t task_a_wakes;
static volatile uint32_t task_b_wakes;

/* --- console -------------------------------------------------------------- */

/* No libc: a local length keeps the image freestanding. */
static uint32_t text_len(const char *s)
{
	uint32_t n = 0;

	while (s[n] != '\0') {
		n++;
	}
	return n;
}

static void console_print(const char *s)
{
	(void)Driver_USART_Console.Send(s, text_len(s));
}

/* --- SPI soft-trigger probe ---------------------------------------------- */

static void spi_probe_handler(void)
{
	IRQ_ClearPending((IRQn_ID_t)SPI_PROBE_INTID);
	spi_hits++;
}

/* --- tasks ---------------------------------------------------------------- */

static void task_a(void *argument)
{
	(void)argument;
	for (;;) {
		osDelay(50);
		task_a_wakes++;
	}
}

static void task_b(void *argument)
{
	(void)argument;
	for (;;) {
		osDelay(30);
		task_b_wakes++;
	}
}

static void task_report(void *argument)
{
	uint32_t seen = 0;

	(void)argument;

	for (;;) {
		osDelay(200);
		if (osKernelGetTickCount() == seen) {
			continue;
		}
		seen = osKernelGetTickCount();

		if (seen >= 500U && task_a_wakes > 0U && task_b_wakes > 0U) {
			console_print("SWITCH OK\n");
			console_print("TICK OK\n");

			if (spi_hits == 0U) {
				console_print("SPI SOFTTRIG FAIL (no handler entry)\n");
			} else {
				console_print("SPI SOFTTRIG OK\n");
			}
			console_print("M0 ANCHORS DONE\n");

			/* Report once, then idle. */
			for (;;) {
				osDelay(1000);
			}
		}
	}
}

/* --- boot ----------------------------------------------------------------- */

void board_main(void)
{
	osKernelState_t state;
	uint32_t cntfrq;

	(void)Driver_USART_Console.Initialize(console_event);
	(void)Driver_USART_Console.PowerControl(ARM_POWER_FULL);

	/* Point the board's fault reporting at the console now that it exists. */
	board_early_print_hook = console_print;

	console_print("\nfreewebcamera M0 - RK3568 FreeRTOS carrier\n");

	/* Report the counter frequency: proves CNTV/CNTFRQ are reachable from
	 * EL1, which is the precondition for the tick working at all. */
	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(cntfrq));
	console_print("cntfrq=");
	{
		char buf[12];
		int i = (int)sizeof(buf) - 1;

		buf[i] = '\0';
		do {
			buf[--i] = (char)('0' + (cntfrq % 10U));
			cntfrq /= 10U;
		} while (cntfrq != 0U && i > 0);
		console_print(&buf[i]);
	}
	console_print("\n");

	/* Interrupt controller, through the CMSIS interface. */
	(void)IRQ_Initialize();

	if (IRQ_SetHandler((IRQn_ID_t)SPI_PROBE_INTID, spi_probe_handler) != 0) {
		console_print("SPI handler install FAIL\n");
	}
	(void)IRQ_SetPriority((IRQn_ID_t)SPI_PROBE_INTID, SPI_PROBE_PRIORITY);
	(void)IRQ_Enable((IRQn_ID_t)SPI_PROBE_INTID);

	/* Kernel init through CMSIS-RTOS2. */
	if (osKernelInitialize() != osOK) {
		console_print("[fatal] osKernelInitialize failed\n");
		return;
	}
	state = osKernelGetState();
	(void)state;

	if (osThreadNew(task_a, 0, &(osThreadAttr_t){ .name = "a",
			.stack_size = 1024, .priority = osPriorityNormal }) == 0) {
		console_print("[fatal] thread a\n");
		return;
	}
	if (osThreadNew(task_b, 0, &(osThreadAttr_t){ .name = "b",
			.stack_size = 1024, .priority = osPriorityNormal }) == 0) {
		console_print("[fatal] thread b\n");
		return;
	}
	if (osThreadNew(task_report, 0, &(osThreadAttr_t){ .name = "report",
			.stack_size = 1024, .priority = osPriorityAboveNormal }) == 0) {
		console_print("[fatal] thread report\n");
		return;
	}

	/* Pend the probe SPI. It stays pending until interrupts are unmasked,
	 * so the first handler entry proves the whole path: distributor enable,
	 * CPU interface, priority, and dispatch. */
	(void)IRQ_SetPending((IRQn_ID_t)SPI_PROBE_INTID);

	/* Never returns. */
	(void)osKernelStart();

	console_print("[fatal] kernel returned\n");
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}
