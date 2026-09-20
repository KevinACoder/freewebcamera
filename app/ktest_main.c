/*
 * @file   ktest_main.c
 * @brief  Entry of the kernel-test image (make KTEST=1 / make ktest).
 *
 * The main image's board_main() (app/main.c) boots the product surface:
 * anchors, shell, network, storage. This image boots the FreeRTOS kernel
 * test suite instead - the official TestRunner suite transplanted under
 * port/adapters/freertos/tests/ - so kernel regressions are caught by
 * flashing a different image, not by editing the product's.
 *
 * Boot shape, and one deliberate difference from the official template: the
 * template's main() goes straight into vStartTests() (which ends in
 * vTaskStartScheduler). Here the shell thread is created first through
 * CMSIS-RTOS2, so a healthy console still offers `ktest` (live per-suite
 * status) while the suite runs; its start task only runs once the suite's
 * vTaskStartScheduler() has started everything.
 *
 * Same layer discipline as app/main.c: this file includes only interface
 * headers (tools/check-deps.sh enforces it) and reaches the test suite
 * through one extern declaration, exactly like the shell adapter reaches
 * smp_selftest().
 */

#include <stdint.h>

#include "Driver_USART.h"
#include "board.h"
#include "cmsis_os2.h"
#include "shell.h"

/* Console handle. Taken from the interface, never from the driver's header. */
extern ARM_DRIVER_USART Driver_USART_Console;

/* port/adapters/freertos/tests/ktest_support.c. Creates the suite's tasks
 * and starts the scheduler; does not return on success. */
extern void kernel_tests_boot(void);

/* Copied from app/main.c: a task rather than a direct call because
 * shell_start() arms the console receive interrupt, whose path ends in an
 * ISR-to-thread flag wake - that needs a running scheduler behind it. */
static void task_shell_start(void *argument)
{
	(void)argument;

	if (shell_start() != 0) {
		board_log("shell: FAIL\n");
		return;
	}
	board_log("shell: READY\n");

	osThreadTerminate(osThreadGetId());
}

void board_main(void)
{
	(void)Driver_USART_Console.Initialize(0);
	(void)Driver_USART_Console.PowerControl(ARM_POWER_FULL);

	board_early_print("\nfreewebcamera ktest - RK3568 kernel test image\n");

	(void)IRQ_Initialize();

	if (osKernelInitialize() != osOK) {
		board_log("fatal: osKernelInitialize failed\n");
		return;
	}

	if (osThreadNew(task_shell_start, 0, &(osThreadAttr_t){
			.name = "shstart", .stack_size = 1024,
			.priority = osPriorityHigh }) == 0) {
		board_log("fatal: thread shell\n");
		return;
	}

	/* Creates the check task and every enabled suite's tasks, then starts
	 * the scheduler (official TestRunner flow). Never returns. */
	kernel_tests_boot();

	board_log("fatal: kernel_tests_boot returned\n");
	for (;;) {
		__asm__ __volatile__("wfe");
	}
}
