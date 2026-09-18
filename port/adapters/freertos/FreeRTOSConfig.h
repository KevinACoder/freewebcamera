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

/* --- SMP ------------------------------------------------------------------ */

/* Four A55 cores, one cluster. The port (port.c / portmacro.h /
 * portasm_smp.S in this directory, transplanted from the board-validated
 * reference line) implements everything the kernel demands when
 * configNUMBER_OF_CORES > 1: core id, cross-core yield, MCS kernel locks,
 * per-core nesting. SMP_CORES arrives from the Makefile (-DSMP_CORES, the
 * single source of truth is board_conf.h) so `make SMP_CORES=1` builds a
 * real single-core comparator image. */
#define configNUMBER_OF_CORES			SMP_CORES
/* V11.3 kernel: affinity APIs exist only when there is more than one core
 * (FreeRTOS.h #errors on the single-core combination). */
#if SMP_CORES > 1
#define configUSE_CORE_AFFINITY			1
#endif

/* Pin each core's idle task to its own core. Without this the idles are
 * created unpinned and migrate: prvYieldCore then marks a FOREIGN core's
 * idle SCHEDULED_TO_YIELD while the scheduler waits for the owning core to
 * switch it, and the per-core bookkeeping (idle on the wrong core, runstate
 * never clearing) wedges the whole machine a few seconds into any multi-core
 * run - observed as "all anchors OK, then every core parks in its idle with
 * the test tasks never scheduled". */
#define configIDLE_AFFINITY			1

/* Cross-priority co-residency. This MUST be 1 on this board: with 0, a core
 * whose only runnable pinned task sits below the GLOBAL highest ready
 * priority may not schedule anything (not even down to idle under the same
 * rule), never re-selects, and never sends or consumes a cross-core yield -
 * a delay task on such a core never wakes. Proven on the reference SMP line
 * (its "轮 17" fix) with a bound delayed task + busy task pair: 0 deadlocks
 * within seconds, 1 runs stable for hours. */
#define configRUN_MULTIPLE_PRIORITIES		1

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

/* The kernel keeps the critical-section nesting count in the TCB (one field
 * per task, indexed through pxCurrentTCBs[core]). */
#define portCRITICAL_NESTING_IN_TCB		1

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

#endif /* FREERTOS_CONFIG_H */
