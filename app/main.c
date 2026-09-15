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

/* Counts failures in the CMSIS-RTOS2 primitive sweep, so the summary line
 * reflects what actually happened instead of always claiming success. */
static uint32_t rtos_check_failures;

/* Defined below, called from the report thread. */
static void rtos_primitives_check(void);

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

			rtos_primitives_check();
			if (rtos_check_failures == 0U) {
				console_print("CMSIS RTOS2 OK\n");
			}

			console_print("M0 ANCHORS DONE\n");

			/* Report once, then idle. */
			for (;;) {
				osDelay(1000);
			}
		}
	}
}

/* --- CMSIS-RTOS2 primitive coverage --------------------------------------- *
 *
 * M0 asks for more than threads and delays: semaphores, queues, mutexes, event
 * flags, timers and a memory pool must all work THROUGH the os* API, because
 * that - not FreeRTOS - is what the rest of the project is allowed to use.
 *
 * Each check is a round trip with a real observable result, not a call that is
 * assumed to have worked: a semaphore is released and acquired, a queue is
 * filled and drained in order, a mutex is taken and given back, event flags are
 * set and seen, a timer is started and waited for, and a pool block is taken
 * and returned. A failure prints which one and why.
 *
 * This runs in the report thread; the timer callback runs in the timer task,
 * which is a different context on purpose.
 */
static volatile uint32_t timer_fired;

static void timer_cb(void *argument)
{
	(void)argument;
	timer_fired++;
}

static void fail(const char *what)
{
	rtos_check_failures++;
	console_print("RTOS CHECK FAIL: ");
	console_print(what);
	console_print("\n");
}

static void rtos_primitives_check(void)
{
	osSemaphoreId_t sem;
	osMessageQueueId_t mq;
	osMutexId_t mtx;
	osEventFlagsId_t ef;
	osTimerId_t tmr;
	osMemoryPoolId_t pool;
	uint32_t v;
	int i;

	/* --- semaphore: release then acquire --- */
	v = 0U;
	sem = osSemaphoreNew(1U, 0U, &(osSemaphoreAttr_t){ .name = "s0" });
	if (sem == NULL) {
		fail("osSemaphoreNew");
	} else {
		if (osSemaphoreRelease(sem) != osOK) {
			fail("osSemaphoreRelease");
		}
		if (osSemaphoreAcquire(sem, 10U) != osOK) {
			fail("osSemaphoreAcquire");
		}
		if (osSemaphoreGetCount(sem) != 0U) {
			fail("osSemaphoreGetCount");
		}
		(void)osSemaphoreDelete(sem);
	}

	/* --- queue: order must survive the round trip --- */
	mq = osMessageQueueNew(4U, sizeof(uint32_t), &(osMessageQueueAttr_t){ .name = "q0" });
	if (mq == NULL) {
		fail("osMessageQueueNew");
	} else {
		for (i = 0; i < 3; i++) {
			v = 100U + (uint32_t)i;
			if (osMessageQueuePut(mq, &v, 0U, 0U) != osOK) {
				fail("osMessageQueuePut");
				break;
			}
		}
		if (osMessageQueueGetCount(mq) != 3U) {
			fail("osMessageQueueGetCount");
		}
		for (i = 0; i < 3; i++) {
			v = 0U;
			if (osMessageQueueGet(mq, &v, NULL, 0U) != osOK) {
				fail("osMessageQueueGet");
				break;
			}
			if (v != 100U + (uint32_t)i) {
				fail("osMessageQueue order");
				break;
			}
		}
		/* The element size was recorded at creation, so it can be
		 * reported back rather than guessed. */
		if (osMessageQueueGetMsgSize(mq) != sizeof(uint32_t)) {
			fail("osMessageQueueGetMsgSize");
		}
		(void)osMessageQueueDelete(mq);
	}

	/* --- mutex: take and return --- */
	mtx = osMutexNew(&(osMutexAttr_t){ .name = "m0" });
	if (mtx == NULL) {
		fail("osMutexNew");
	} else {
		if (osMutexAcquire(mtx, 10U) != osOK) {
			fail("osMutexAcquire");
		}
		if (osMutexGetOwner(mtx) != osThreadGetId()) {
			fail("osMutexGetOwner");
		}
		if (osMutexRelease(mtx) != osOK) {
			fail("osMutexRelease");
		}
		(void)osMutexDelete(mtx);
	}

	/* --- event flags: set, then read back --- */
	ef = osEventFlagsNew(&(osEventFlagsAttr_t){ .name = "e0" });
	if (ef == NULL) {
		fail("osEventFlagsNew");
	} else {
		if (osEventFlagsSet(ef, 0x5U) != 0x5U) {
			fail("osEventFlagsSet");
		}
		if ((osEventFlagsGet(ef) & 0x5U) != 0x5U) {
			fail("osEventFlagsGet");
		}
		(void)osEventFlagsDelete(ef);
	}

	/* --- timer: one-shot, callback in the timer task --- */
	timer_fired = 0U;
	tmr = osTimerNew(timer_cb, osTimerOnce, NULL, &(osTimerAttr_t){ .name = "t0" });
	if (tmr == NULL) {
		fail("osTimerNew");
	} else {
		if (osTimerStart(tmr, 50U) != osOK) {
			fail("osTimerStart");
		}
		/* Real wait for the callback, not an assumption. */
		for (i = 0; i < 20 && timer_fired == 0U; i++) {
			osDelay(25U);
		}
		if (timer_fired == 0U) {
			fail("osTimer callback never ran");
		}
		(void)osTimerDelete(tmr);
	}

	/* --- memory pool: allocate, write, free --- */
	pool = osMemoryPoolNew(4U, 32U, &(osMemoryPoolAttr_t){ .name = "p0" });
	if (pool == NULL) {
		fail("osMemoryPoolNew");
	} else {
		uint32_t *blk = (uint32_t *)osMemoryPoolAlloc(pool, 10U);

		if (blk == NULL) {
			fail("osMemoryPoolAlloc");
		} else {
			blk[0] = 0xfeedfaceU;
			if (blk[0] != 0xfeedfaceU) {
				fail("osMemoryPool block not writable");
			}
			if (osMemoryPoolGetCount(pool) != 1U) {
				fail("osMemoryPoolGetCount");
			}
			if (osMemoryPoolFree(pool, blk) != osOK) {
				fail("osMemoryPoolFree");
			}
			if (osMemoryPoolGetCount(pool) != 0U) {
				fail("osMemoryPoolGetCount after free");
			}
		}
		(void)osMemoryPoolDelete(pool);
	}

	/* --- thread flags: set on self and read back --- */
	if (osThreadFlagsSet(osThreadGetId(), 0x3U) != 0x3U) {
		fail("osThreadFlagsSet");
	}
	if ((osThreadFlagsGet() & 0x3U) != 0x3U) {
		fail("osThreadFlagsGet");
	}
	if ((osThreadFlagsClear(0x1U) & 0x3U) != 0x3U) {
		fail("osThreadFlagsClear return value");
	}
	if ((osThreadFlagsGet() & 0x1U) != 0U) {
		fail("osThreadFlagsClear did not clear");
	}

	console_print("CMSIS RTOS2 DONE\n");
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
