/*
 * FreeRTOS Kernel V11.2.0
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
 * freewebcamera adaptation (D33): board-validated SMP port transplanted from
 * the reference SDK line that runs stable on this exact hardware; the
 * integrator-side calls map onto the board layer:
 *   InterruptSetPriorityMask/GetPriorityMask -> IRQ_SetPriorityMask /
 *       IRQ_GetPriorityMask (CMSIS irq_ctrl.h; ICC_PMR_EL1 under the hood).
 *       The reference tree conditions these on a measured RPR "translate"
 *       step; on this board the step measured 16 (four priority bits, value
 *       N lives in the byte as N << 4), so the translation is the identity
 *       and is baked out - see vPortPriorityConfigCheck.
 *   InterruptSetPriority/Install/Umask -> IRQ_SetPriority/IRQ_SetHandler/
 *       IRQ_Enable (the raw-byte value comes from board.h policy).
 *   FGetCpuLogicalId/FCpuIdMapInit -> board_smp_core_id() (board.h). The
 *       reference tree keeps a boot-order logical map for multi-cluster
 *       parts; RK3568 is a single cluster whose cores number themselves in
 *       MPIDR Aff1 (measured 0x8100_0N00) and the boot core is always Aff1 0
 *       under U-Boot `go`, so the map is the identity and is baked out.
 *   FGetCpuPhysicalId -> not needed: the PSCI CPU_ON target encoding is the
 *       linear core index on this board (board_conf.h).
 *   DbgRawPrintStr/U32 -> board_early_print (polled UART, spinlock, DAIF
 *       masked while held - same discipline, board-owned).
 * Registered in IMPORT-INFO.md / docs/imports.md.
 */

/* Standard includes. */
#include <stdint.h>
#include <stddef.h>
/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "irq_ctrl.h"

#ifndef configINTERRUPT_CONTROLLER_BASE_ADDRESS
    #error "configINTERRUPT_CONTROLLER_BASE_ADDRESS must be defined. See www.FreeRTOS.org/Using-FreeRTOS-on-Cortex-A-Embedded-Processors.html"
#endif

#ifndef configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET
    #error "configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET must be defined.  See www.FreeRTOS.org/Using-FreeRTOS-on-Cortex-A-Embedded-Processors.html"
#endif

#ifndef configUNIQUE_INTERRUPT_PRIORITIES
    #error "configUNIQUE_INTERRUPT_PRIORITIES must be defined.  See www.FreeRTOS.org/Using-FreeRTOS-on-Cortex-A-Embedded-Processors.html"
#endif

#ifndef configSETUP_TICK_INTERRUPT
    #error "configSETUP_TICK_INTERRUPT() must be defined.  See www.FreeRTOS.org/Using-FreeRTOS-on-Cortex-A-Embedded-Processors.html"
#endif /* configSETUP_TICK_INTERRUPT */

#ifndef configMAX_API_CALL_INTERRUPT_PRIORITY
    #error "configMAX_API_CALL_INTERRUPT_PRIORITY must be defined.  See www.FreeRTOS.org/Using-FreeRTOS-on-Cortex-A-Embedded-Processors.html"
#endif

#if configMAX_API_CALL_INTERRUPT_PRIORITY == 0
    #error "configMAX_API_CALL_INTERRUPT_PRIORITY must not be set to 0"
#endif

#if configMAX_API_CALL_INTERRUPT_PRIORITY > configUNIQUE_INTERRUPT_PRIORITIES
    #error "configMAX_API_CALL_INTERRUPT_PRIORITY must be less than or equal to configUNIQUE_INTERRUPT_PRIORITIES as the lower the numeric priority value the higher the logical interrupt priority"
#endif

#if configUSE_PORT_OPTIMISED_TASK_SELECTION == 1
    /* Check the configuration. */
    #if ( configMAX_PRIORITIES > 32 )
        #error "configUSE_PORT_OPTIMISED_TASK_SELECTION can only be set to 1 when configMAX_PRIORITIES is less than or equal to 32.  It is very rare that a system requires more than 10 to 15 difference priorities as tasks that share a priority will time slice."
    #endif
#endif /* configUSE_PORT_OPTIMISED_TASK_SELECTION */

/* In case security extensions are implemented. */
#if configMAX_API_CALL_INTERRUPT_PRIORITY <= ( configUNIQUE_INTERRUPT_PRIORITIES / 2 )
    #error "configMAX_API_CALL_INTERRUPT_PRIORITY must be greater than ( configUNIQUE_INTERRUPT_PRIORITIES / 2 )"
#endif

/* Some vendor specific files default configCLEAR_TICK_INTERRUPT() in
 * portmacro.h. */
#ifndef configCLEAR_TICK_INTERRUPT
    #define configCLEAR_TICK_INTERRUPT()
#endif

/* A critical section is exited when the critical section nesting count reaches
 * this value. */
#define portNO_CRITICAL_NESTING          ( ( size_t ) 0 )

/* In all GICs 255 can be written to the priority mask register to unmask all
 * (but the lowest) interrupt priority. */
#define portUNMASK_VALUE                 ( 0xFFUL )

/* Tasks are not created with a floating point context, but can be given a
 * floating point context after they have been created.  A variable is stored as
 * part of the tasks context that holds portNO_FLOATING_POINT_CONTEXT if the task
 * does not have an FPU context, or any other value if the task does have an FPU
 * context. */
#define portNO_FLOATING_POINT_CONTEXT    ( ( StackType_t ) 0 )

/* Constants required to setup the initial task context. */
#define portSP_ELx                       ( ( StackType_t ) 0x01 )
#define portSP_EL0                       ( ( StackType_t ) 0x00 )

#if defined( GUEST )
    #define portEL1                      ( ( StackType_t ) 0x04 )
    #define portINITIAL_PSTATE           ( portEL1 | portSP_EL0 )
#else
    #define portEL3                      ( ( StackType_t ) 0x0c )
    /* At the time of writing, the BSP only supports EL3. */
    #define portINITIAL_PSTATE           ( portEL3 | portSP_EL0 )
#endif


/* Used by portASSERT_IF_INTERRUPT_PRIORITY_INVALID() when ensuring the binary
 * point is zero. */
#define portBINARY_POINT_BITS      ( ( uint8_t ) 0x03 )

/* Masks all bits in the APSR other than the mode bits. */
#define portAPSR_MODE_BITS_MASK    ( 0x0C )

/* The I bit in the DAIF bits. */
#define portDAIF_I                 ( 0x80 )

/* Macro to unmask all interrupt priorities. The PMR write is a single MSR;
 * deliberately NO DAIF manipulation here (see uxPortSetInterruptMask - a
 * blind DAIFCLR in ISR context lets level-triggered lines nest into their
 * own critical sections). */
#define portUMASK_INTERRUPT()         \
    {                                      \
        IRQ_SetPriorityMask(portUNMASK_VALUE); \
        __asm volatile("DSB SY		\n"        \
                       "ISB SY		\n");  \
    }

/* Hardware specifics used when sanity checking the configuration. */
#define portINTERRUPT_PRIORITY_REGISTER_OFFSET    0x400UL
#define portMAX_8_BIT_VALUE                       ( ( uint8_t ) 0xff )
#define portBIT_0_SET                             ( ( uint8_t ) 0x01 )

/* ICC_BPR1_EL1 (S3_0_C12_C12_3) and ICC_RPR_EL1 (S3_0_C12_C11_3), read
 * directly: the CPU-interface system registers need no driver.
 *
 * The RPR encoding is not a typo magnet to be casual with: the first draft
 * of this port used S3_0_C12_C8_0 - which is ICC_IAR0, the GROUP 0
 * acknowledge register. An NS EL1 read of a Group 0 interface register
 * traps into EL3, where OP-TEE's unexpected-trap path takes the machine
 * down with no non-secure-side print at all (the same silent mechanism as
 * the ICC_SRE_EL2 write in smp_secondary.S). Board signature: instant warm
 * reset at the FIRST FromISR API call that validates priority, everything
 * before it green. */
static inline uint32_t icc_bpr1_read( void )
{
    uint32_t value;

    __asm volatile( "mrs %0, s3_0_c12_c12_3" : "=r"( value ) :: "memory" );
    return value;
}

static inline uint32_t icc_rpr_read( void )
{
    uint32_t value;

    __asm volatile( "mrs %0, s3_0_c12_c11_3" : "=r"( value ) :: "memory" );
    return value;
}

/* Let the user override the pre-loading of the initial LR with the address of
 * prvTaskExitError() in case it messes up unwinding of the stack in the
 * debugger. */
#ifdef configTASK_RETURN_ADDRESS
    #define portTASK_RETURN_ADDRESS    configTASK_RETURN_ADDRESS
#else
    #define portTASK_RETURN_ADDRESS    prvTaskExitError
#endif


/*-----------------------------------------------------------*/

/*
 * Starts the first task executing.  This function is necessarily written in
 * assembly code so is implemented in portASM.s.
 */
extern void vPortRestoreTaskContext( void );

/*
 * Used to catch tasks that attempt to return from their implementing function.
 */
static void prvTaskExitError( void );

/*-----------------------------------------------------------*/

/* A variable is used to keep track of the critical section nesting.  This
 * variable has to be stored as part of the task context and must be initialised to
 * a non zero value to ensure interrupts don't inadvertently become unmasked before
 * the scheduler starts.  As it is stored as part of the task context it will
 * automatically be set to 0 when the first task is started. */
volatile uint64_t ullCriticalNesting = 9999ULL;

/* Saved as part of the task context.  If ullPortTaskHasFPUContext is non-zero
 * then floating point context must be saved and restored for the task. */
uint64_t ullPortTaskHasFPUContext = pdFALSE;

/* Set to 1 to pend a context switch from an ISR. */
uint64_t ullPortYieldRequired = pdFALSE;

/* Counts the interrupt nesting depth.  A context switch is only performed if
 * if the nesting depth is 0. */
uint64_t ullPortInterruptNesting = 0;


/* The space on the stack required to hold the FPU registers.  This is 32 128-bit
 * registers, that means (64 * 8) 64 double words */
#define portFPU_REGISTER_DOUBLE_WORDS ( 64 )

/* Used in the ASM code. */
volatile uint64_t ullMaxAPIPriorityMask = (configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT);

volatile uint64_t ullPortUnmask = portUNMASK_VALUE;

volatile uint8_t ucPriorityConfig = 0;

static void vPortPriorityConfigCheck(void);

/* Out-of-line core id for the assembly paths (portasm.S cannot reach the
 * static inline in board.h with a BL). */
uint32_t vPortGetCoreID( void )
{
    return board_smp_core_id();
}

/*-----------------------------------------------------------*/

/*
 * See header file for description.
 */
StackType_t * pxPortInitialiseStack( StackType_t * pxTopOfStack,
                                     TaskFunction_t pxCode,
                                     void * pvParameters )
{
    /* Setup the initial stack of a task. The stack is set exactly as
     * expected by the portRESTORE_CONTEXT() macro. */

    /* First all the general purpose registers. */
    pxTopOfStack--;
    *pxTopOfStack = 0x0101010101010101ULL;        /* R1 */
    pxTopOfStack--;
    *pxTopOfStack = ( StackType_t ) pvParameters; /* R0 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0303030303030303ULL;        /* R3 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0202020202020202ULL;        /* R2 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0505050505050505ULL;        /* R5 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0404040404040404ULL;        /* R4 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0707070707070707ULL;        /* R7 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0606060606060606ULL;        /* R6 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0909090909090909ULL;        /* R9 */
    pxTopOfStack--;
    *pxTopOfStack = 0x0808080808080808ULL;        /* R8 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1111111111111111ULL;        /* R11 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1010101010101010ULL;        /* R10 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1313131313131313ULL;        /* R13 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1212121212121212ULL;        /* R12 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1515151515151515ULL;        /* R15 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1414141414141414ULL;        /* R14 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1717171717171717ULL;        /* R17 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1616161616161616ULL;        /* R16 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1919191919191919ULL;        /* R19 */
    pxTopOfStack--;
    *pxTopOfStack = 0x1818181818181818ULL;        /* R18 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2121212121212121ULL;        /* R21 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2020202020202020ULL;        /* R20 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2323232323232323ULL;        /* R23 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2222222222222222ULL;        /* R22 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2525252525252525ULL;        /* R25 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2424242424242424ULL;        /* R24 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2727272727272727ULL;        /* R27 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2626262626262626ULL;        /* R26 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2929292929292929ULL;        /* R29 */
    pxTopOfStack--;
    *pxTopOfStack = 0x2828282828282828ULL;        /* R28 */
    pxTopOfStack--;
    *pxTopOfStack = ( StackType_t ) 0x00;         /* XZR - has no effect, used so there are an even number of registers. */
    pxTopOfStack--;
    *pxTopOfStack = (StackType_t)portTASK_RETURN_ADDRESS; /* R30 - procedure call link register. */
    pxTopOfStack--;

    *pxTopOfStack = portINITIAL_PSTATE;
    pxTopOfStack--;

    *pxTopOfStack = ( StackType_t ) pxCode; /* Exception return address. */
    pxTopOfStack--;

#if( configUSE_TASK_FPU_SUPPORT == 1 )
    {
        /* The task will start with a critical nesting count of 0 as interrupts are
            enabled. */
        *pxTopOfStack = portNO_CRITICAL_NESTING;
        /* The task will start without a floating point context.  A task that uses
        the floating point hardware must call vPortTaskUsesFPU() before executing
        any floating point instructions. */
        pxTopOfStack--;
        *pxTopOfStack = portNO_FLOATING_POINT_CONTEXT;
    }
#elif( configUSE_TASK_FPU_SUPPORT == 2 )
    {
        /* The task will start with a floating point context. Leave enough
         * space for the registers - and ensure they are initialised to 0. */
        pxTopOfStack -= (portFPU_REGISTER_DOUBLE_WORDS - 1);
        memset(pxTopOfStack, 0x00, portFPU_REGISTER_DOUBLE_WORDS * sizeof(StackType_t));

        /* The task will start with a critical nesting count of 0 as interrupts are
            enabled. */
        pxTopOfStack--;
        *pxTopOfStack = portNO_CRITICAL_NESTING;
        pxTopOfStack--;
        *pxTopOfStack = pdTRUE;
        ullPortTaskHasFPUContext = pdTRUE;
    }
#else
    {
#error "Invalid configUSE_TASK_FPU_SUPPORT setting - configUSE_TASK_FPU_SUPPORT must be set to 1, 2, or left undefined."
    }
#endif

    return pxTopOfStack;
}

static void prvTaskExitError( void )
{
    /* A function that implements a task must not exit or attempt to return to
     * its caller as there is nothing to return to.  If a task wants to exit it
     * should instead call vTaskDelete( NULL ).
     *
     * Artificially force an assert() to be triggered if configASSERT() is
     * defined, then stop here so application writers can catch the error. */
    portDISABLE_INTERRUPTS();

    for( ; ; )
    {
    }
}

/*-----------------------------------------------------------*/

BaseType_t xPortStartScheduler(void)
{
    uint32_t ulAPSR;

    /* The reference tree builds a boot-order logical core map here
     * (FCpuIdMapInit); on this single-cluster board the logical number is
     * MPIDR Aff1 read directly (board_smp_core_id), so there is nothing to
     * build. See the file header. */

    /* check gic configuration */
    if (portGET_CORE_ID() == 0)
    {
        vPortPriorityConfigCheck();
    }

    /* At the time of writing, the BSP only supports EL1. */
    __asm volatile("MRS %0, CurrentEL"
                   : "=r"(ulAPSR));
    ulAPSR &= portAPSR_MODE_BITS_MASK;

    #if defined( GUEST )
        configASSERT( ulAPSR == portEL1 );

        if( ulAPSR == portEL1 )
    #else
        configASSERT( ulAPSR == portEL3 );

        if( ulAPSR == portEL3 )
    #endif
    {
        /* Only continue if the binary point value is set to its lowest possible
         * setting.  See the comments in vPortValidateInterruptPriority() below for
         * more information. */
        configASSERT((icc_bpr1_read() & portBINARY_POINT_BITS) <= portMAX_BINARY_POINT_VALUE);

        if ((icc_bpr1_read() & portBINARY_POINT_BITS) <= portMAX_BINARY_POINT_VALUE)
        {
            /* Interrupts are turned off in the CPU itself to ensure a tick does
             * not execute while the scheduler is being started.  Interrupts are
             * automatically turned back on in the CPU when the first task starts
             * executing. */
            portDISABLE_INTERRUPTS();

            /* Start the timer that generates the tick ISR. */
            if (portGET_CORE_ID() == 0)
            {
                configSETUP_TICK_INTERRUPT();
            }

            /* Start the first task executing. */
            vPortRestoreTaskContext();
        }
    }

    return 0;
}
/*-----------------------------------------------------------*/

void vPortEndScheduler( void )
{
    /* Not implemented in ports where there is nothing to return to.
     * Artificially force an assert. */
    configASSERT( 0 );
}
/*-----------------------------------------------------------*/

void vPortEnterCritical( void )
{
    /* Mask interrupts up to the max syscall interrupt priority. */
    uxPortSetInterruptMask();

    /* Now interrupts are disabled ullCriticalNesting can be accessed
     * directly.  Increment ullCriticalNesting to keep a count of how many times
     * portENTER_CRITICAL() has been called. */
    ullCriticalNesting++;

    /* This is not the interrupt safe version of the enter critical function so
     * assert() if it is being called from an interrupt context.  Only API
     * functions that end in "FromISR" can be used in an interrupt.  Only assert if
     * the critical nesting count is 1 to protect against recursive calls if the
     * assert function also uses a critical section. */
    if( ullCriticalNesting == 1ULL )
    {
        configASSERT( ullPortInterruptNesting == 0 );
    }
}
/*-----------------------------------------------------------*/

void vPortExitCritical( void )
{
    if( ullCriticalNesting > portNO_CRITICAL_NESTING )
    {
        /* Decrement the nesting count as the critical section is being
         * exited. */
        ullCriticalNesting--;

        /* If the nesting level has reached zero then all interrupt
         * priorities must be re-enabled. */
        if( ullCriticalNesting == portNO_CRITICAL_NESTING )
        {
            /* Critical nesting has reached zero so all interrupt priorities
             * should be unmasked. */
            portUMASK_INTERRUPT();
        }
    }
}
/*-----------------------------------------------------------*/
/*
 * Global counter used for calculation of run time statistics of tasks.
 * Defined only when the relevant option is turned on
 */
#if (configGENERATE_RUN_TIME_STATS==1)
static volatile uint64_t ulHighFrequencyTimerTicks = 0;
#endif

void FreeRTOS_Tick_Handler( void )
{

    /* Interrupts should not be enabled before this point. */
    #if ( configASSERT_DEFINED == 1 )
    {
        uint32_t ulMaskBits;

        __asm volatile ( "mrs %0, daif" : "=r" ( ulMaskBits )::"memory" );
        configASSERT( ( ulMaskBits & portDAIF_I ) != 0 );
    }
    #endif /* configASSERT_DEFINED */

    /* Set interrupt mask before altering scheduler structures.   The tick
     * handler runs at the lowest priority, so interrupts cannot already be masked,
     * so there is no need to save and restore the current mask value.  It is
     * necessary to turn off interrupts in the CPU itself while the ICCPMR is being
     * updated. */

    IRQ_SetPriorityMask(configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT);
    __asm volatile("dsb sy		\n"
                   "isb sy		\n" ::
                   : "memory");

    /* Increment the RTOS tick. */
    if (xTaskIncrementTick() != pdFALSE)
    {
        ullPortYieldRequired = pdTRUE;
    }

    /* unmask all interrupt priorities. */
    IRQ_SetPriorityMask(portUNMASK_VALUE);

    /* interrupt clear. */
    configCLEAR_TICK_INTERRUPT();

}
/*-----------------------------------------------------------*/

void vPortTaskUsesFPU( void )
{
    /* A task is registering the fact that it needs an FPU context.  Set the
     * FPU flag (which is saved as part of the task context). */
    /* Consider initialising the FPSR here - but probably not necessary in
     * AArch64. */
    ullPortTaskHasFPUContext = pdTRUE;
}
/*-----------------------------------------------------------*/

void vPortClearInterruptMask( UBaseType_t uxNewMaskValue )
{
    if( uxNewMaskValue == pdFALSE )
    {
        portUMASK_INTERRUPT();
    }
}

/*-----------------------------------------------------------*/

UBaseType_t uxPortSetInterruptMask( void )
{
    uint32_t ulReturn;
    unsigned long ulDaif;

    /* Save the caller's whole DAIF. The reference tree ends this helper with
     * a blind portENABLE_INTERRUPTS(); on THIS integration that is fatal:
     * FromISR paths run with IRQs masked by exception entry, and re-enabling
     * them here lets the very line being serviced (level-triggered GMAC
     * lines, board-proven 2026-09-18) nest into its own FromISR critical
     * section. The PMR does the masking work; the caller's DAIF state must
     * be preserved exactly. */
    __asm volatile( "mrs %0, daif" : "=r"( ulDaif ) :: "memory" );

    /* Interrupt in the CPU must be turned off while the ICCPMR is being
     * updated. */
    portDISABLE_INTERRUPTS();
    if (IRQ_GetPriorityMask() == (uint32_t)(configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT))
    {
        /* Interrupts were already masked. */
        ulReturn = pdTRUE;
    }
    else
    {
        ulReturn = pdFALSE;
        IRQ_SetPriorityMask(configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT);
        __asm volatile("dsb sy		\n"
                       "isb sy		\n" ::
                       : "memory");
    }

    __asm volatile( "msr daif, %0" :: "r"( ulDaif ) : "memory" );

    return ulReturn;
}
/*-----------------------------------------------------------*/

#if ( configASSERT_DEFINED == 1 )

    void vPortValidateInterruptPriority( void )
    {
        /* The following assertion will fail if a service routine (ISR) for
         * an interrupt that has been assigned a priority above
         * configMAX_SYSCALL_INTERRUPT_PRIORITY calls an ISR safe FreeRTOS API
         * function.  ISR safe FreeRTOS API functions must *only* be called
         * from interrupts that have been assigned a priority at or below
         * configMAX_SYSCALL_INTERRUPT_PRIORITY.
         *
         * Numerically low interrupt priority numbers represent logically high
         * interrupt priorities, therefore the priority of the interrupt must
         * be set to a value equal to or numerically *higher* than
         * configMAX_SYSCALL_INTERRUPT_PRIORITY.
         *
         * FreeRTOS maintains separate thread and ISR API functions to ensure
         * interrupt entry is as fast and simple as possible. */

        configASSERT(icc_rpr_read() >= (uint32_t)(configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT));

        /* Priority grouping:  The interrupt controller (GIC) allows the bits
         * that define each interrupt's priority to be split between bits that
         * define the interrupt's pre-emption priority bits and bits that define
         * the interrupt's sub-priority.  For simplicity all bits must be defined
         * to be pre-emption priority bits.  The following assertion will fail if
         * this is not the case (if some bits represent a sub-priority).
         *
         * The priority grouping is configured by the GIC's binary point register
         * (ICCBPR).  Writting 0 to ICCBPR will ensure it is set to its lowest
         * possible value (which may be above 0). */
        configASSERT((icc_bpr1_read() & portBINARY_POINT_BITS) <= portMAX_BINARY_POINT_VALUE);
}

#endif /* configASSERT_DEFINED */
/*-----------------------------------------------------------*/

void vTaskSwitchSPx(void)
{
    board_early_print("Switch task in interrupt is error.");
    configASSERT(0);
}

extern int vApplicationInIrq(void);
__attribute__((weak)) int xPortIsInsideInterrupt( void )
{
    return vApplicationInIrq();
}

static void vPortPriorityConfigCheck(void)
{
    /* The reference tree reads back a measured RPR step here and, when the
     * firmware left the GIC in an 8-bit-priority configuration, translates
     * every PMR value written through the assembly-side constants below. On
     * THIS board the same measurement yields step 16 - four priority bits,
     * logical level N in the byte as N << 4 - so the translation is the
     * identity and there is nothing to apply. The variable is kept (and
     * exported for diagnostics) because the assembly constant path exists
     * either way. */
    ucPriorityConfig = 0;
}
