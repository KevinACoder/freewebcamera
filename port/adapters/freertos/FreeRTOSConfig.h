/*
 * @file   FreeRTOSConfig.h
 * @brief  Kernel configuration for the RK3568 carrier.
 *
 * Everything here is dictated by two things: the upstream ARM_AARCH64_SRE port
 * (which has hard requirements, see below) and the board's interrupt model.
 *
 * Port requirements that are NOT optional:
 *  - configSETUP_TICK_INTERRUPT() must be defined; port.c #errors without it.
 *  - configMAX_API_CALL_INTERRUPT_PRIORITY must be defined, non-zero, <=
 *    configUNIQUE_INTERRUPT_PRIORITIES, and strictly greater than half of it.
 *    port.c rejects several combinations at compile time.
 *  - configUNIQUE_INTERRUPT_PRIORITIES == 16 selects portPRIORITY_SHIFT == 4,
 *    which is what this GIC-600 implements (4 priority bits, 16 levels).
 *
 * The priority numbers are raw hardware values. The tick must run at the
 * lowest usable priority: FreeRTOS_Tick_Handler asserts that the running
 * priority equals portLOWEST_USABLE_INTERRUPT_PRIORITY, so a tick at any other
 * priority trips the assertion. Any interrupt that calls a FromISR API must be
 * configured at configMAX_API_CALL_INTERRUPT_PRIORITY instead, or
 * vPortValidateInterruptPriority fires.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* The board owns interrupt-priority policy; this config derives from it so the
 * two cannot drift apart. board.h has no kernel dependency, so including it
 * here creates no cycle. */
#include "board.h"

/* --- scheduler ------------------------------------------------------------ */

#define configUSE_PREEMPTION			1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION	1
#define configUSE_TICKLESS_IDLE			0

#define configCPU_CLOCK_HZ			24000000UL
#define configTICK_RATE_HZ			1000U
#define configMAX_PRIORITIES			8
#define configMINIMAL_STACK_SIZE		512
#define configMAX_TASK_NAME_LEN			16
#define configUSE_16_BIT_TICKS			0
#define configIDLE_SHOULD_YIELD			0

/* --- memory --------------------------------------------------------------- */

/* heap_4 over the region the link script reserves (__heap_start). */
#define configSUPPORT_STATIC_ALLOCATION		0
#define configSUPPORT_DYNAMIC_ALLOCATION	1
#define configTOTAL_HEAP_SIZE			(1024U * 1024U)
#define configAPPLICATION_ALLOCATED_HEAP	1

/* --- hooks and checks ----------------------------------------------------- */

#define configUSE_IDLE_HOOK			0
#define configUSE_TICK_HOOK			0
#define configUSE_MALLOC_FAILED_HOOK		1
#define configUSE_DAEMON_TASK_STARTUP_HOOK	0
#define configCHECK_FOR_STACK_OVERFLOW		2
#define configUSE_TRACE_FACILITY		1

/* Needed by the CMSIS-RTOS2 adapter: thread stack high-water mark and mutex
 * owner queries are behind these switches. */
#define INCLUDE_uxTaskGetStackHighWaterMark	1
#define INCLUDE_xSemaphoreGetMutexHolder	1
#define INCLUDE_xTaskGetCurrentTaskHandle	1
#define INCLUDE_xTaskGetSchedulerState		1
#define INCLUDE_eTaskGetState			1
#define INCLUDE_xTimerPendFunctionCall		0
#define INCLUDE_vTaskDelete			1
#define INCLUDE_vTaskPrioritySet		1
#define INCLUDE_vTaskSuspend			1
#define INCLUDE_vTaskDelay			1
#define INCLUDE_xTaskDelayUntil			1
#define INCLUDE_xTaskGetIdleTaskHandle		0
#define INCLUDE_pcTaskGetTaskName		1
#define INCLUDE_uxTaskPriorityGet		1
#define INCLUDE_xTaskAbortDelay			0
#define INCLUDE_xTaskGetHandle			0

/* Disabled during bring-up: a failed assert must be visible, but the
 * FullFreeRTOSConfig asserts fire from interrupt context where the console is
 * polled and slow. Re-enable once the system is stable. */
#define configASSERT_DEFINED			1

/* --- optional features ---------------------------------------------------- */

#define configUSE_MUTEXES			1
#define configUSE_RECURSIVE_MUTEXES		1
#define configUSE_COUNTING_SEMAPHORES		1
#define configUSE_QUEUE_SETS			0
#define configUSE_TASK_NOTIFICATIONS		1
#define configUSE_TIMERS			1
#define configTIMER_TASK_PRIORITY		7
#define configTIMER_QUEUE_LENGTH		16
#define configTIMER_TASK_STACK_DEPTH		512
#define configUSE_EVENT_GROUPS			1
#define configUSE_STREAM_BUFFERS		0

/* --- interrupt priorities (raw hardware values) --------------------------- */

#define configUNIQUE_INTERRUPT_PRIORITIES	16

/* Interrupts at or below (numerically) this level may call FromISR APIs.
 * Derived from the board's policy so the two cannot disagree. */
#define configMAX_API_CALL_INTERRUPT_PRIORITY	BOARD_IRQ_PRIORITY_API_CALL

/* configKERNEL_INTERRUPT_PRIORITY is deliberately NOT defined. It is a
 * Cortex-M/R-style setting that this AArch64 SRE port never reads (grep the
 * port directory: zero uses) - the tick's priority reaches the hardware
 * through configSETUP_TICK_INTERRUPT below and the board's own
 * BOARD_IRQ_PRIORITY_TICK_RAW. Defining it here would look authoritative while
 * having no effect, which is exactly the kind of setting worth not having. */

/* --- the port's required hooks -------------------------------------------- */

/* Declarations for the two hooks the port calls from its own .c file. Without
 * these, port.c sees implicit declarations (the macros below expand inside it)
 * and the build warns. */
void board_tick_port_setup(void);
void OS_Tick_AcknowledgeIRQ(void);

/* Called from xPortStartScheduler to arm the tick. Implemented in
 * port/adapters/freertos/port_glue.c, which drives the board's CNTV/INTID 27
 * through OS_Tick_Setup. */
#define configSETUP_TICK_INTERRUPT() \
	board_tick_port_setup()

#define configCLEAR_TICK_INTERRUPT() \
	OS_Tick_AcknowledgeIRQ()

/* The port's IRQ entry reads ICC_IAR1_EL1 and then calls this with the INTID;
 * the port performs the EOI itself. */
void vApplicationIRQHandler(uint32_t ulICCAck);

/* Declared rather than included: this header is consumed by FreeRTOS.h before
 * task.h is available, so pulling in task.h here would be circular. */
struct tskTaskControlBlock;
void vApplicationMallocFailedHook(void);
void vApplicationStackOverflowHook(struct tskTaskControlBlock *xTask,
				   char *pcTaskName);

#define configASSERT(x) \
	do { if ((x) == 0) { board_assert_failed(__FILE__, __LINE__); } } while (0)

void board_assert_failed(const char *file, int line);

#endif /* FREERTOS_CONFIG_H */
