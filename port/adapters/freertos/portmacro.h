/*
 * FreeRTOS Kernel V11.1.0
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 * freewebcamera adaptation (D33): the SMP port is the board-validated
 * implementation proven on this hardware by the reference SDK line; the
 * integrator-supplied pieces map onto this tree's board layer as follows:
 *   FGetCpuLogicalId()   -> board_smp_core_id()   (board.h; MPIDR Aff1 here)
 *   InterruptSetPriority -> IRQ_SetPriority       (CMSIS irq_ctrl.h, raw byte)
 *   vInterruptCore()     -> port_glue.c           -> board_gicv3_send_sgi()
 * Priority values follow the board's policy (board.h): 16 unique levels,
 * logical level N lives in the register byte as N << 4. Registered in
 * IMPORT-INFO.md / docs/imports.md.
 */

#ifndef PORTMACRO_H
#define PORTMACRO_H

/* *INDENT-OFF* */
#ifdef __cplusplus
    extern "C" {
#endif
/* *INDENT-ON* */
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "FreeRTOSConfig.h"
/*-----------------------------------------------------------
 * Port specific definitions.
 *
 * The settings in this file configure FreeRTOS correctly for the given hardware
 * and compiler.
 *
 * These settings should not be altered.
 *-----------------------------------------------------------
 */

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
#if( configNUMBER_OF_CORES == 1 )

#define portEND_SWITCHING_ISR( xSwitchRequired ) \
    {                                            \
        extern uint64_t ullPortYieldRequired;    \
                                                 \
        if( xSwitchRequired != pdFALSE )         \
        {                                        \
            ullPortYieldRequired = pdTRUE;       \
        }                                        \
    }

#else

/* Task utilities. */
extern uint64_t ullPortYieldRequired[];
/* Called at the end of an ISR that can cause a context switch. */
#define portEND_SWITCHING_ISR( xSwitchRequired ) \
    {                                            \
                                                 \
        if( xSwitchRequired != pdFALSE )         \
        {                                        \
            ullPortYieldRequired[portGET_CORE_ID()] = pdTRUE;       \
        }                                        \
    }

#endif

#define portYIELD_FROM_ISR( x )    portEND_SWITCHING_ISR( x )

#if defined( GUEST )
    #define portYIELD()            __asm volatile ( "SVC 0" ::: "memory" )
#else
    #define portYIELD()            __asm volatile ( "SMC 0" ::: "memory" )
#endif

/*-----------------------------------------------------------
* Critical section control
*----------------------------------------------------------*/


extern UBaseType_t uxPortSetInterruptMask( void );
extern void vPortClearInterruptMask( UBaseType_t uxNewMaskValue );
extern void vPortInstallFreeRTOSVectorTable( void );

#define portDISABLE_INTERRUPTS()                       \
    __asm volatile ( "MSR DAIFSET, #2" ::: "memory" ); \
    __asm volatile ( "DSB SY" );                       \
    __asm volatile ( "ISB SY" );

#define portENABLE_INTERRUPTS()                        \
    __asm volatile ( "MSR DAIFCLR, #2" ::: "memory" ); \
    __asm volatile ( "DSB SY" );                       \
    __asm volatile ( "ISB SY" );


static inline UBaseType_t uxDisableInterrupts()
{
    unsigned long flags;

    __asm volatile (
        "mrs %0, daif\n"
        "msr daifset, #2\n"
        : "=r" (flags)
        :
        : "memory"
    );

    return flags;
}

static inline void vRestoreInterrupts(UBaseType_t flags)
{
    __asm volatile (
        "and x2, %0, #128\n"
        "mrs x1, daif\n"
        "bic x1, x1, #128\n"
        "orr x1, x1, x2\n"
        "msr daif, x1\n"
        :
        : "r" (flags)
        : "x0","x1","x2","memory"
    );
}

#define portSET_INTERRUPT_MASK_FROM_ISR()         uxPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR( x )    vPortClearInterruptMask( x )

#if( configNUMBER_OF_CORES == 1 )

extern void vPortEnterCritical( void );
extern void vPortExitCritical( void );

/* These macros do not globally disable/enable interrupts.  They do mask off
 * interrupts that have a priority below configMAX_API_CALL_INTERRUPT_PRIORITY. */
#define portENTER_CRITICAL()                      vPortEnterCritical();
#define portEXIT_CRITICAL()                       vPortExitCritical();


#else /* #if( configNUMBER_OF_CORES == 1 ) */

#define portENTER_CRITICAL()		            vTaskEnterCritical();
#define portEXIT_CRITICAL()			            vTaskExitCritical();

#define portSET_INTERRUPT_MASK()                uxDisableInterrupts()
#define portCLEAR_INTERRUPT_MASK(x)             vRestoreInterrupts(x)

#define portENTER_CRITICAL_FROM_ISR()           vTaskEnterCriticalFromISR()
#define portEXIT_CRITICAL_FROM_ISR( x )         vTaskExitCriticalFromISR( x )

/*-----------------------------------------------------------
 * Critical section locks
 *----------------------------------------------------------*/
#define ISR_LOCK                (0u)
#define TASK_LOCK               (1u)

extern void vPortRecursiveLock(BaseType_t xCoreID, uint32_t ulLockNum, BaseType_t uxAcquire);

/* Per-core lock state initialisation. */
void vPortLockInit(void);

/* Task lock interface. */
void vPortTaskLock(BaseType_t coreId, BaseType_t acquire);

/* ISR lock interface. */
void vPortISRLock(BaseType_t coreId, BaseType_t acquire);

#define portRELEASE_ISR_LOCK( xCoreID )     vPortISRLock(( xCoreID ), pdFALSE)
#define portGET_ISR_LOCK( xCoreID )         vPortISRLock(( xCoreID ), pdTRUE)

#define portRELEASE_TASK_LOCK( xCoreID )    vPortTaskLock(( xCoreID ), pdFALSE)
#define portGET_TASK_LOCK( xCoreID )        vPortTaskLock(( xCoreID ), pdTRUE)

#endif

/*-----------------------------------------------------------*/

/* Task function macros as described on the FreeRTOS.org WEB site.  These are
 * not required for this port but included in case common demo code that uses these
 * macros is used. */
#define portTASK_FUNCTION_PROTO( vFunction, pvParameters )    void vFunction( void * pvParameters )
#define portTASK_FUNCTION( vFunction, pvParameters )          void vFunction( void * pvParameters )

/* Prototype of the FreeRTOS tick handler.  This must be installed as the
 * handler for whichever peripheral is used to generate the RTOS tick. */
void FreeRTOS_Tick_Handler( void );

/* Any task that uses the floating point unit MUST call vPortTaskUsesFPU()
 * before any floating point instructions are executed. */
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

#ifdef configASSERT
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

/* Interrupt controller access addresses. */
#define portICCPMR_PRIORITY_MASK_OFFSET                      ( 0x04 )
#define portICCIAR_INTERRUPT_ACKNOWLEDGE_OFFSET              ( 0x0C )
#define portICCEOIR_END_OF_INTERRUPT_OFFSET                  ( 0x10 )
#define portICCBPR_BINARY_POINT_OFFSET                       ( 0x08 )
#define portICCRPR_RUNNING_PRIORITY_OFFSET                   ( 0x14 )


#define portMEMORY_BARRIER()    __asm volatile ( "" ::: "memory" )

/* Symmetric MultiProcessing (SMP) utility */
#if ( configNUMBER_OF_CORES > 1 )

static inline BaseType_t xPortGetCoreID()
{
   register BaseType_t xCoreID;
   xCoreID = (BaseType_t)board_smp_core_id();
   return xCoreID;
}

/* port for SMP */
#define portGET_CORE_ID()       xPortGetCoreID()

/* Assembly paths need an out-of-line symbol; the static inline above cannot
 * be reached with BL. port.c provides it on top of the same board primitive. */
extern uint32_t vPortGetCoreID( void );

extern void vInterruptCore(uint32_t ulInterruptID, uint32_t ulCoreID);
/* Use sgi0 as the yield core interrupt. */
#define portYIELD_CORE_INT_ID       0
#define portYIELD_CORE( xCoreID )   vInterruptCore(portYIELD_CORE_INT_ID, (uint32_t)xCoreID)


int xPortIsInsideInterrupt( void );

#endif

/* *INDENT-OFF* */
#ifdef __cplusplus
    }
#endif
/* *INDENT-ON* */

#endif /* PORTMACRO_H */
