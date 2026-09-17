/*
 * @file   portmacro.h
 * @brief  SMP port macros for the RK3568 (four A55, EL1/GUEST, GIC-600).
 *
 * Derived from the vendored upstream single-core port
 * (third-party/FreeRTOS-Kernel/portable/GCC/ARM_AARCH64_SRE/portmacro.h,
 * MIT) and extended with the pieces the V11.3 kernel requires when
 * configNUMBER_OF_CORES > 1. The kernel, not this file, defines the
 * contract; see FreeRTOS.h lines around "required in SMP". Registered as a
 * derivative of the vendored MIT port in IMPORT-INFO.md / imports.md.
 *
 * What changed against the single-core port, and why:
 *
 *  - portGET_CORE_ID(): MPIDR Aff0 through the board's static inline. One
 *    MRS, no function call, valid on every path that needs it.
 *
 *  - portYIELD_CORE(): the cross-core yield. The kernel calls this from
 *    inside its own critical sections when a task on ANOTHER core must be
 *    preempted; on THIS core it just pends xYieldPendings[] instead. The
 *    SGI's handler only sets a per-core flag, so it may run above the
 *    API-call priority level (BOARD_IRQ_PRIORITY_SGI).
 *
 *  - portGET/RELEASE_TASK_LOCK + ISR_LOCK: two global spinlocks (task and
 *    ISR). The kernel's own discipline pairs every acquire with a release
 *    and releases-then-reacquires around yields, so a plain non-recursive
 *    spinlock is sufficient - and keeping interrupts fully masked in the
 *    paths that hold them (see port_smp.c) means no same-core re-entry can
 *    occur. The xCoreID parameter exists for ports that track per-core
 *    recursion; this one does not need it and ignores it.
 *
 *  - Critical sections: the kernel's vTaskEnterCritical/vTaskExitCritical
 *    (DAIF + both locks + TCB-resident nesting count), NOT the single-core
 *    port's PMR-based vPortEnterCritical. taskENTER_CRITICAL maps here
 *    unconditionally (task.h), so this file's macros must match.
 *
 *  - portSET/CLEAR_INTERRUPT_MASK (and the _FROM_ISR forms): unchanged
 *    PMR semantics from the single-core port; the kernel uses them around
 *    uxSchedulerSuspended updates and xTaskGetCurrentTaskHandle.
 *
 *  - portENTER/EXIT_CRITICAL_FROM_ISR: the kernel's FromISR critical
 *    sections (ISR lock + PMR save), which the V11.3 kernel provides.
 *
 *  - portEND_SWITCHING_ISR: per-core yield-required array.
 *
 * EL vocabulary is unchanged (GUEST): tasks run at EL1, yield is SVC 0,
 * vectors live at VBAR_EL1, initial pstate is EL1 with SP_EL0.
 */

#ifndef PORTMACRO_H
#define PORTMACRO_H

#include "board.h"

/* *INDENT-OFF* */
#ifdef __cplusplus
    extern "C" {
#endif
/* *INDENT-ON* */

/* Type definitions. */
#define portCHAR          char
#define portFLOAT         float
#define portDOUBLE        double
#define portLONG          long
#define portSHORT         short
#define portSTACK_TYPE    size_t
#define portBASE_TYPE     long

typedef portSTACK_TYPE   StackType_t;
typedef portBASE_TYPE    BaseType_t;
typedef uint64_t         UBaseType_t;

typedef uint64_t         TickType_t;
#define portMAX_DELAY              ( ( TickType_t ) 0xffffffffffffffff )

/* 32-bit tick type on a 32-bit architecture, so reads of the tick count do
 * not need to be guarded with a critical section. */
#define portTICK_TYPE_IS_ATOMIC    1

/*-----------------------------------------------------------*/

/* Hardware specifics. */
#define portSTACK_GROWTH         ( -1 )
#define portTICK_PERIOD_MS       ( ( TickType_t ) 1000 / configTICK_RATE_HZ )
#define portBYTE_ALIGNMENT       16
#define portPOINTER_SIZE_TYPE    uint64_t

/*-----------------------------------------------------------*/

/* The number of cores this port runs on, and the INTID of the cross-core
 * yield SGI. configNUMBER_OF_CORES is derived from the same value, so the
 * kernel and the port cannot disagree (FreeRTOSConfig.h). */
#define portNUM_CORES               BOARD_SMP_CORES
#define portYIELD_SGI_INTID         BOARD_SMP_YIELD_INTID

/* The kernel keeps the critical-section nesting count in the TCB (one field
 * per task, indexed through pxCurrentTCBs[core]); the port keeps no copy of
 * its own, and the asm context switch carries none - a task is only ever
 * switched with a zero count. */
#define portCRITICAL_NESTING_IN_TCB    1

/* Task utilities. */

/* Called at the end of an ISR that can cause a context switch. Per-core:
 * the switch is pended for whichever core is running the ISR. */
#define portEND_SWITCHING_ISR( xSwitchRequired )                      \
    {                                                                 \
        extern uint64_t ullPortYieldRequired[ portNUM_CORES ];        \
                                                                      \
        if( ( xSwitchRequired ) != pdFALSE )                          \
        {                                                             \
            ullPortYieldRequired[ portGET_CORE_ID() ] = pdTRUE;       \
        }                                                             \
    }

#define portYIELD_FROM_ISR( x )    portEND_SWITCHING_ISR( x )
#define portYIELD()                __asm volatile ( "SVC 0" ::: "memory" )

/* Cross-core yield: SGI to the target core. Called by the kernel with its
 * own critical section held, so the send must work with interrupts masked
 * (an SGI register write does). */
#define portYIELD_CORE( xCoreID ) \
    board_gicv3_send_sgi( portYIELD_SGI_INTID, 1UL << ( xCoreID ) )

/* Core identification: one MRS. */
#define portGET_CORE_ID()    ( ( BaseType_t ) board_smp_core_id() )

/*-----------------------------------------------------------
* Critical section control
*----------------------------------------------------------*/

/* Interrupt masking is unchanged from the single-core port: PMR-based,
 * per-core by GICv3 architecture. */
extern void vPortClearInterruptMask( UBaseType_t uxNewMaskValue );
extern UBaseType_t uxPortSetInterruptMask( void );

/* Task-level critical sections are the kernel's SMP implementation: DAIF
 * mask, task+ISR spinlocks, nesting count in the TCB. Prototypes here
 * because these macros expand in files that include only FreeRTOS.h. */
extern void vTaskEnterCritical( void );
extern void vTaskExitCritical( void );
extern UBaseType_t vTaskEnterCriticalFromISR( void );
extern void vTaskExitCriticalFromISR( UBaseType_t uxSavedInterruptStatus );

#define portDISABLE_INTERRUPTS()                       \
    __asm volatile ( "MSR DAIFSET, #2" ::: "memory" ); \
    __asm volatile ( "DSB SY" );                       \
    __asm volatile ( "ISB SY" );

#define portENABLE_INTERRUPTS()                        \
    __asm volatile ( "MSR DAIFCLR, #2" ::: "memory" ); \
    __asm volatile ( "DSB SY" );                       \
    __asm volatile ( "ISB SY" );

#define portENTER_CRITICAL()                      vTaskEnterCritical()
#define portEXIT_CRITICAL()                       vTaskExitCritical()

/* The kernel's scheduler-masking calls (vTaskSuspendAll et al). */
#define portSET_INTERRUPT_MASK()                   uxPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK( x )              vPortClearInterruptMask( x )

/* ISR paths keep the single-core semantics. */
#define portSET_INTERRUPT_MASK_FROM_ISR()          uxPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR( x )     vPortClearInterruptMask( x )

/* FromISR critical sections: kernel-provided (ISR lock + PMR save). */
#define portENTER_CRITICAL_FROM_ISR()              vTaskEnterCriticalFromISR()
#define portEXIT_CRITICAL_FROM_ISR( x )            vTaskExitCriticalFromISR( x )

/* Kernel locks: two global spinlocks (see the file comment). The core id is
 * accepted and ignored - it exists for ports that track recursion per core,
 * which this one does not (no holder can re-enter: every path that holds a
 * lock runs with interrupts fully masked). */
extern void uxPortTaskLock( void );
extern void uxPortTaskUnlock( void );
extern void uxPortISRLock( void );
extern void uxPortISRUnlock( void );

#define portGET_TASK_LOCK( xCoreID )           uxPortTaskLock()
#define portRELEASE_TASK_LOCK( xCoreID )       uxPortTaskUnlock()
#define portGET_ISR_LOCK( xCoreID )            uxPortISRLock()
#define portRELEASE_ISR_LOCK( xCoreID )        uxPortISRUnlock()

/* The IRQ entry counts nesting depth per core; asserting "not in ISR" reads
 * this core's depth. */
extern uint64_t ullPortInterruptNesting[ portNUM_CORES ];

#if ( configASSERT_DEFINED == 1 )
    #define portASSERT_IF_IN_ISR()                                            \
        configASSERT( ullPortInterruptNesting[ portGET_CORE_ID() ] == 0U )
#endif /* configASSERT_DEFINED */

/*-----------------------------------------------------------*/

/* Task function macros as described on the FreeRTOS.org WEB site.  These
 * are not required for this port but included in case common demo code that
 * uses these macros is used. */
#define portTASK_FUNCTION_PROTO( vFunction, pvParameters )    void vFunction( void * pvParameters )
#define portTASK_FUNCTION( vFunction, pvParameters )          void vFunction( void * pvParameters )

/* Prototype of the FreeRTOS tick handler.  This must be installed as the
 * handler for whichever peripheral is used to generate the RTOS tick. */
void FreeRTOS_Tick_Handler( void );

/* If configUSE_TASK_FPU_SUPPORT is set to 1 (or left undefined) then tasks
 * are created without an FPU context and must call vPortTaskUsesFPU() to
 * give themselves an FPU context before using any FPU instructions. */
void vPortTaskUsesFPU( void );
#define portTASK_USES_FLOATING_POINT()    vPortTaskUsesFPU()

#define portLOWEST_INTERRUPT_PRIORITY           ( ( ( uint32_t ) configUNIQUE_INTERRUPT_PRIORITIES ) - 1UL )
#define portLOWEST_USABLE_INTERRUPT_PRIORITY    ( portLOWEST_INTERRUPT_PRIORITY - 1UL )

/* Architecture specific optimisations. */
#ifndef configUSE_PORT_OPTIMISED_TASK_SELECTION
    #define configUSE_PORT_OPTIMISED_TASK_SELECTION    1
#endif

#if configUSE_PORT_OPTIMISED_TASK_SELECTION == 1

/* Store/clear the ready priorities in a bit map. */
    #define portRECORD_READY_PRIORITY( uxPriority, uxReadyPriorities )    ( uxReadyPriorities ) |= ( 1UL << ( uxPriority ) )
    #define portRESET_READY_PRIORITY( uxPriority, uxReadyPriorities )     ( uxReadyPriorities ) &= ~( 1UL << ( uxPriority ) )

/*-----------------------------------------------------------*/

    #define portGET_HIGHEST_PRIORITY( uxTopPriority, uxReadyPriorities )    uxTopPriority = ( 31 - __builtin_clz( uxReadyPriorities ) )

#endif /* configUSE_PORT_OPTIMISED_TASK_SELECTION */

#if ( configASSERT_DEFINED == 1 )
    void vPortValidateInterruptPriority( void );
    #define portASSERT_IF_INTERRUPT_PRIORITY_INVALID()    vPortValidateInterruptPriority()
#endif /* configASSERT */

#define portNOP()                                         __asm volatile ( "NOP" )
#define portINLINE    __inline

/* The number of bits to shift for an interrupt priority is dependent on the
 * number of bits implemented by the interrupt controller. */
#if configUNIQUE_INTERRUPT_PRIORITIES == 16
    #define portPRIORITY_SHIFT            4
    #define portMAX_BINARY_POINT_VALUE    3
#elif configUNIQUE_INTERRUPT_PRIORITIES == 32
    #define portPRIORITY_SHIFT            3
    #define portMAX_BINARY_POINT_VALUE    2
#elif configUNIQUE_INTERRUPT_PRIORITIES == 64
    #define portPRIORITY_SHIFT            2
    #define portMAX_BINARY_POINT_VALUE    1
#elif configUNIQUE_INTERRUPT_PRIORITIES == 128
    #define portPRIORITY_SHIFT            1
    #define portMAX_BINARY_POINT_VALUE    0
#elif configUNIQUE_INTERRUPT_PRIORITIES == 256
    #define portPRIORITY_SHIFT            0
    #define portMAX_BINARY_POINT_VALUE    0
#else /* if configUNIQUE_INTERRUPT_PRIORITIES == 16 */
    #error Invalid configUNIQUE_INTERRUPT_PRIORITIES setting.  configUNIQUE_INTERRUPT_PRIORITIES must be set to the number of unique priorities implemented by the target hardware
#endif /* if configUNIQUE_INTERRUPT_PRIORITIES == 16 */

#define portMEMORY_BARRIER()    __asm volatile ( "dsb sy" ::: "memory" )

/* *INDENT-OFF* */
#ifdef __cplusplus
    }
#endif
/* *INDENT-ON* */

#endif /* PORTMACRO_H */
