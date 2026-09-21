/*
 * @file   FreeRTOSConfig.h
 * @brief  Kernel configuration for the RK3568 carrier.
 *
 * Everything here is dictated by two things: the SMP port's hard requirements
 * (see below) and the board's interrupt model. The port is the
 * board-validated implementation transplanted from the reference SDK line
 * (D33, IMPORT-INFO.md); its non-negotiables:
 *
 *  - configSETUP_TICK_INTERRUPT() must be defined; port.c #errors without it.
 *  - configINTERRUPT_CONTROLLER_BASE_ADDRESS / _CPU_INTERFACE_OFFSET are
 *    compile-time checked (the SRE-style port drives the GIC through system
 *    registers, so the values are documentation + a guard, not MMIO).
 *  - configMAX_API_CALL_INTERRUPT_PRIORITY must be defined, non-zero, <=
 *    configUNIQUE_INTERRUPT_PRIORITIES, and strictly greater than half of it.
 *  - configUNIQUE_INTERRUPT_PRIORITIES == 16 selects portPRIORITY_SHIFT == 4,
 *    which is what this GIC-600 implements (4 priority bits, 16 levels).
 *
 * The priority numbers follow the board's policy (board.h) and the reference
 * line's board-validated values: the tick at 13, the API-call ceiling at 15.
 * Any interrupt that calls a FromISR API must be configured at
 * configMAX_API_CALL_INTERRUPT_PRIORITY or higher numerically, or
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
/* The ready-priority bitmap is single-core-only (the kernel #errors when
 * configNUMBER_OF_CORES > 1); task selection uses the list walk instead. */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION	0
#define configUSE_TICKLESS_IDLE			0

/* --- core count ----------------------------------------------------------- */

/* Single core, by decision D42: ThreadX SMP is the mainline kernel; this
 * FreeRTOS port is the single-core support/comparator image only (the SMP
 * port that served D32/D33 was removed with it). The BOARD still boots
 * SMP_CORES cores when the image is the ThreadX one - SMP_CORES here only
 * selects the board's tick source (CNTV/27 single-core, CNTPNS/30 SMP), and
 * the FreeRTOS build is pinned to SMP_CORES=1 in the Makefile. */
#define configNUMBER_OF_CORES			1

#define configCPU_CLOCK_HZ			24000000UL
#define configTICK_RATE_HZ			1000U
/* The test suite creates ~100 tasks across priorities up to
 * configMAX_PRIORITIES - 2 (the reference line ran it with 32 levels); the
 * main image's product tasks need nowhere near that. */
#ifdef KTEST_BUILD
#define configMAX_PRIORITIES			16
#else
#define configMAX_PRIORITIES			8
#endif
#define configMINIMAL_STACK_SIZE		512
#define configMAX_TASK_NAME_LEN			16
#define configUSE_16_BIT_TICKS			0
#define configIDLE_SHOULD_YIELD			0

/* --- memory --------------------------------------------------------------- */

/* heap_4 over the region the link script reserves (__heap_start). Static
 * allocation support (and the kernel's own provider for the idle/timer
 * task storage behind configKERNEL_PROVIDED_STATIC_MEMORY) exists for the
 * StaticAllocation suite and the static task paths; dynamic allocation
 * stays the project's working default. */
#define configSUPPORT_STATIC_ALLOCATION		1
#define configKERNEL_PROVIDED_STATIC_MEMORY	1
#define configSUPPORT_DYNAMIC_ALLOCATION	1
/* The test image needs roughly 4x: the full suite keeps ~100 tasks alive
 * (each with a configMINIMAL_STACK_SIZE stack) plus queues, timers and
 * event groups, all out of heap_4. The reference line ran the same suite
 * with a 20 MiB heap. The main image keeps the bring-up-era 1 MiB. */
#ifdef KTEST_BUILD
#define configTOTAL_HEAP_SIZE			(4096U * 1024U)
#else
#define configTOTAL_HEAP_SIZE			(1024U * 1024U)
#endif
#define configAPPLICATION_ALLOCATED_HEAP	1

/* --- hooks and checks ----------------------------------------------------- */

#define configUSE_IDLE_HOOK			0
/* The test suite's official TestRunner.c defines vApplicationTickHook() and
 * drives half of its ISR-side demos from it (task notifications from ISR,
 * queue overwrite, event-group processing, IntQueue). Only the ktest image
 * links that file, so only that image enables the hook. */
#ifdef KTEST_BUILD
#define configUSE_TICK_HOOK			1
#else
#define configUSE_TICK_HOOK			0
#endif
/* SMP kernel idle tasks: one per core, each may need the hook slot defined
 * (the kernel #errors without a definition, hook or not). */
#define configUSE_PASSIVE_IDLE_HOOK		0
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
/* The EventGroupsDemo suite drives the event group from the tick hook via
 * xEventGroupSetBitsFromISR, which the kernel compiles only behind this
 * switch (it routes through the timer daemon's pending-function queue). */
#define INCLUDE_xTimerPendFunctionCall		1
#define INCLUDE_vTaskDelete			1
#define INCLUDE_vTaskPrioritySet		1
#define INCLUDE_vTaskSuspend			1
#define INCLUDE_vTaskDelay			1
#define INCLUDE_xTaskDelayUntil			1
/* The three below were 0 during bring-up and are needed by the kernel test
 * suite (death/dynamic use the idle handle and xTaskGetHandle; the
 * AbortDelay and GenQTest extended tests are behind INCLUDE_xTaskAbortDelay,
 * whose #error the build would hit). Cost is unused kernel code in the main
 * image only. */
#define INCLUDE_xTaskGetIdleTaskHandle		1
#define INCLUDE_pcTaskGetTaskName		1
#define INCLUDE_uxTaskPriorityGet		1
#define INCLUDE_xTaskAbortDelay			1
#define INCLUDE_xTaskGetHandle			1

/* Disabled during bring-up: a failed assert must be visible, but the
 * FullFreeRTOSConfig asserts fire from interrupt context where the console is
 * polled and slow. Re-enable once the system is stable. */
#define configASSERT_DEFINED			1

/* --- optional features ---------------------------------------------------- */

#define configUSE_MUTEXES			1
#define configUSE_RECURSIVE_MUTEXES		1
#define configUSE_COUNTING_SEMAPHORES		1
/* Queue sets and abort-delay: off during M-line bring-up, on since the
 * kernel test suite needs them (QueueSet/QueueSetPolling/GenQTest/AbortDelay
 * suites #error without these). Small code-size cost, no behaviour change
 * for code that does not use them. */
#define configUSE_QUEUE_SETS			1
#define configUSE_TASK_NOTIFICATIONS		1
/* TaskNotifyArray suite needs at least three indexed notifications. */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES	3
#define configUSE_TIMERS			1
/* Derived rather than literal so the daemon stays the top priority at any
 * configMAX_PRIORITIES (the TimerDemo suite assumes it is not starved). */
#define configTIMER_TASK_PRIORITY		( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH		16
#define configTIMER_TASK_STACK_DEPTH		512
#define configUSE_EVENT_GROUPS			1
/* The ktest image needs the stream buffer API: the upstream AbortDelay test
 * verifies xTaskAbortDelay against a stream buffer as one of its block
 * primitives. The StreamBuffer/MessageBuffer DEMOS stay excluded (single-core
 * assumptions, see tests_config.h) - this only compiles the API in. */
#ifdef KTEST_BUILD
#define configUSE_STREAM_BUFFERS		1
#else
#define configUSE_STREAM_BUFFERS		0
#endif

/* --- interrupt priorities (raw hardware values) --------------------------- */

#define configUNIQUE_INTERRUPT_PRIORITIES	16

/* Interrupts at or below (numerically) this level may call FromISR APIs.
 * Derived from the board's policy so the two cannot disagree: 14, the
 * highest SIGNABLE level on this GIC (15 is not signable - see board.h for
 * the four-bit-PMR arithmetic and the board boot that proved it). Every
 * FromISR driver (console, GMAC, ITS LPIs) sits at exactly this level. */
#define configMAX_API_CALL_INTERRUPT_PRIORITY	BOARD_IRQ_PRIORITY_API_CALL

/* The tick's logical priority, also board policy (BOARD_IRQ_PRIORITY_TICK).
 * The port passes configKERNEL_INTERRUPT_PRIORITY semantics through the
 * board's tick setup (tick.c programs the raw byte); kept defined so the
 * kernel-port contract is visible in one place. */
#define configKERNEL_INTERRUPT_PRIORITY		BOARD_IRQ_PRIORITY_TICK

/* The port's compile-time guard wants the controller's coordinates; on this
 * system-register-interface port they are never dereferenced. */
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS	BOARD_GICD_BASE
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET	0x2000UL

/* FPU context policy of the port's initial stack frame: 1 = tasks start
 * without FP context and must call vPortTaskUsesFPU() first (the build is
 * -mgeneral-regs-only, so nothing ever will - the flag just fixes the frame
 * shape). */
#define configUSE_TASK_FPU_SUPPORT		1

/* --- the port's required hooks -------------------------------------------- */

/* Declarations for the two hooks the port calls from its own .c file. Without
 * these, port.c sees implicit declarations (the macros below expand inside it)
 * and the build warns. */
void board_tick_port_setup(void);
void OS_Tick_AcknowledgeIRQ(void);

/* Called from xPortStartScheduler to arm the tick. Implemented in
 * port/adapters/freertos/port_glue.c, which drives the board's selected
 * tick timer through OS_Tick_Setup (CNTV/INTID 27 single-core,
 * CNTPNS/INTID 30 under SMP - see board_conf.h). */
#define configSETUP_TICK_INTERRUPT() \
	board_tick_port_setup()

#define configCLEAR_TICK_INTERRUPT() \
	OS_Tick_AcknowledgeIRQ()

/* Declared rather than included: this header is consumed by FreeRTOS.h before
 * task.h is available, so pulling in task.h here would be circular. */
struct tskTaskControlBlock;
void vApplicationMallocFailedHook(void);
void vApplicationStackOverflowHook(struct tskTaskControlBlock *xTask,
				   char *pcTaskName);

#define configASSERT(x) \
	do { if ((x) == 0) { board_assert_failed(__FILE__, __LINE__); } } while (0)

void board_assert_failed(const char *file, int line);

/* The test suite's print macro (official ThirdParty-Template convention:
 * called as configPRINTF(( "fmt", args ))). The board's formatted log is
 * already spinlock-serialized and safe from any core, which is exactly the
 * cross-core console discipline the reference line learned the hard way
 * (raw printf from multiple cores corrupts newlib's stdio state). */
#define configPRINTF( X )	board_log X

#endif /* FREERTOS_CONFIG_H */
