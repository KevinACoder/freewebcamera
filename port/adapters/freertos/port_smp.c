/*
 * @file   port_smp.c
 * @brief  SMP FreeRTOS port for the RK3568 (four A55, EL1/GUEST, GIC-600).
 *
 * Derived from the vendored upstream single-core port
 * (third-party/FreeRTOS-Kernel/portable/GCC/ARM_AARCH64_SRE/port.c, MIT) and
 * extended with the SMP pieces the V11.3 kernel requires. Registered as a
 * derivative in IMPORT-INFO.md / imports.md. The context-save shape, the
 * initial task stack layout, the PMR masking helpers and the priority
 * assertions are the upstream port's, unchanged in meaning; everything
 * per-core is new.
 *
 * ---------------------------------------------------------------------------
 * HOW THIS PORT RELATES TO THE KERNEL'S SMP DESIGN
 *
 *  - Critical sections belong to the kernel: vTaskEnterCritical masks DAIF,
 *    takes the task and ISR spinlocks (first entry only) and keeps the
 *    nesting count in the TCB. Context switches only ever happen with
 *    nesting zero, so a task is always switched in with DAIF clear and PMR
 *    wide open - the asm restores PMR to 0xff unconditionally and keeps no
 *    per-task interrupt state.
 *
 *  - There is one tick, on one core. xTaskIncrementTick() mutates the
 *    scheduler lists but does NOT lock; taking the ISR lock around it is the
 *    port's job (FreeRTOS_Tick_Handler). The handler never re-enables
 *    interrupts mid-flight - unlike the single-core handler - because a
 *    nested ISR calling a FromISR API would try to take the ISR lock this
 *    handler already holds.
 *
 *  - Cross-core preemption travels as SGI 0: the kernel's prvYieldCore()
 *    calls portYIELD_CORE(), which writes ICC_SGI1R_EL1 (board layer); the
 *    handler on the target core only sets that core's
 *    ullPortYieldRequired[], and the IRQ exit path performs the switch.
 *    Because the handler calls no kernel API, the SGI may run above the
 *    API-call priority (BOARD_IRQ_PRIORITY_SGI) and therefore stays
 *    deliverable while the target sits in a PMR-narrowed critical section.
 *
 *  - Each core enters the scheduler separately. The boot core releases the
 *    secondaries through PSCI, waits for their GIC bring-up to report in,
 *    then arms the tick and sets uxPortSchedulerRunning. A secondary - after
 *    its own per-core init - spins on that flag and then restores
 *    pxCurrentTCBs[core], which the kernel already pointed at that core's
 *    idle task during vTaskStartScheduler.
 * ---------------------------------------------------------------------------
 */

/* Standard includes. */
#include <stdlib.h>

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"

/* Board primitives: core id, SGI send, per-core GIC init, CMSIS irq_ctrl. */
#include "board.h"
#include "irq_ctrl.h"

#ifndef configUNIQUE_INTERRUPT_PRIORITIES
    #error "configUNIQUE_INTERRUPT_PRIORITIES must be defined."
#endif

#ifndef configSETUP_TICK_INTERRUPT
    #error "configSETUP_TICK_INTERRUPT() must be defined."
#endif

#ifndef configMAX_API_CALL_INTERRUPT_PRIORITY
    #error "configMAX_API_CALL_INTERRUPT_PRIORITY must be defined."
#endif

/* In case security extensions are implemented. */
#if configMAX_API_CALL_INTERRUPT_PRIORITY <= ( configUNIQUE_INTERRUPT_PRIORITIES / 2 )
    #error "configMAX_API_CALL_INTERRUPT_PRIORITY must be greater than ( configUNIQUE_INTERRUPT_PRIORITIES / 2 )"
#endif

/* Some vendor specific files default configCLEAR_TICK_INTERRUPT() in
 * portmacro.h. */
#ifndef configCLEAR_TICK_INTERRUPT
    #define configCLEAR_TICK_INTERRUPT()
#endif

/* A critical section is exited when the critical section nesting count
 * reaches this value. Kept for the initial-stack layout: the slot is pushed
 * with this value, and (unlike the single-core port) never read back - in
 * SMP the kernel keeps the nesting count in the TCB. */
#define portNO_CRITICAL_NESTING          ( ( size_t ) 0 )

/* In all GICs 255 can be written to the priority mask register to unmask all
 * (but the lowest) interrupt priority. */
#define portUNMASK_VALUE                 ( 0xFFUL )

/* Tasks are not created with a floating point context, but can be given a
 * floating point context after they have been created. */
#define portNO_FLOATING_POINT_CONTEXT    ( ( StackType_t ) 0 )

/* Constants required to setup the initial task context. */
#define portSP_ELx                       ( ( StackType_t ) 0x01 )
#define portSP_EL0                       ( ( StackType_t ) 0x00 )

/* GUEST build: tasks run at EL1 with SP_EL0 selected. */
#define portEL1                          ( ( StackType_t ) 0x04 )
#define portINITIAL_PSTATE               ( portEL1 | portSP_EL0 )

/* Masks all bits in the APSR other than the mode bits. */
#define portAPSR_MODE_BITS_MASK    ( 0x0C )

/* The I bit in the DAIF bits. */
#define portDAIF_I                 ( 0x80 )

/* Macro to unmask all interrupt priorities. Named differently from the
 * kernel-contract portCLEAR_INTERRUPT_MASK(x) in portmacro.h, which is a
 * save/restore-style call - this one is the bare PMR primitive underneath. */
/* s3_0_c4_c6_0 is ICC_PMR_EL1. */
#define portPMR_UNMASK_ALL()                           \
    {                                                  \
        __asm volatile ( "MSR DAIFSET, #2        \n"   \
                         "DSB SY                 \n"   \
                         "ISB SY                 \n"   \
                         "MSR s3_0_c4_c6_0, %0   \n"   \
                         "DSB SY                 \n"   \
                         "ISB SY                 \n"   \
                         "MSR DAIFCLR, #2        \n"   \
                         "DSB SY                 \n"   \
                         "ISB SY                 \n"   \
                         ::"r" ( portUNMASK_VALUE ) ); \
    }

/* The space on the stack required to hold the FPU registers.
 * There are 32 128-bit plus 2 64-bit status registers.*/
#define portFPU_REGISTER_WORDS     ( (32 * 2) + 2 )

/*-----------------------------------------------------------*/

/*
 * Starts the first task executing.  Written in assembly (portasm_smp.S).
 */
extern void vPortRestoreTaskContext( void );

/*
 * Secondary-core C entry, called from smp_secondary.S once the core is at
 * EL1 with its MMU on. Defined below.
 */
void uxPortSecondaryMain( void );

/*
 * If the application provides an implementation of vApplicationIRQHandler(),
 * then it will get called directly without saving the FPU registers on
 * interrupt entry. See the vendored port for the full contract - kept
 * identical here.
 */
void vApplicationFPUSafeIRQHandler( uint32_t ulICCIAR ) __attribute__((weak) );

/*-----------------------------------------------------------*/

/* Set to 1 to pend a context switch, PER CORE: an ISR only ever pends its
 * own core. Read and cleared by the asm IRQ-exit path. */
uint64_t ullPortYieldRequired[ portNUM_CORES ];

/* Counts the interrupt nesting depth per core. A context switch is only
 * performed if the nesting depth is 0. */
uint64_t ullPortInterruptNesting[ portNUM_CORES ];

/* Saved as part of the task context (per task, per core): non-zero when the
 * running task owns an FPU context. Always 0 with -mgeneral-regs-only. */
uint64_t ullPortTaskHasFPUContext[ portNUM_CORES ];

/* Set by the boot core once the tick is armed and the scheduler is about to
 * start. Secondaries spin here before their first context restore. */
volatile uint64_t uxPortSchedulerRunning = pdFALSE;

/* Used in the ASM code: the PMR value that masks everything at or below the
 * API-call level. */
__attribute__( ( used ) ) const uint64_t ullMaxAPIPriorityMask = ( configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT );

/*-----------------------------------------------------------*/

/* --- kernel locks: two global spinlocks ----------------------------------- */

static volatile unsigned int task_lock_held;
static volatile unsigned int isr_lock_held;

/* Written in assembly on purpose: __atomic_test_and_set on a 4-byte object
 * compiles to a call into libatomic (undefined in a -nostdlib image), while
 * the LDAXR/STXR pair is two instructions and needs no runtime. Returns 0
 * when the lock was taken. */
static inline unsigned int smp_lock_try( volatile unsigned int * lock )
{
    unsigned int status;
    unsigned int wanted = 1u;

    __asm volatile (
        "   ldaxr  %w0, %2      \n"
        "   cbnz   %w0, 1f      \n"
        "   stxr   %w0, %w1, %2 \n"
        "1:                     \n"
        : "=&r" ( status )
        : "r" ( wanted ), "Q" ( * ( volatile unsigned int * ) lock )
        : "memory" );

    return status;
}

static void smp_lock_acquire( volatile unsigned int * lock )
{
    while( smp_lock_try( lock ) != 0u )
    {
        do
        {
            __asm volatile ( "wfe" ::: "memory" );
        } while( *lock != 0u );
    }
}

static void smp_lock_release( volatile unsigned int * lock )
{
    /* STLR: store with release semantics, the pairing half of LDAXR. */
    __asm volatile ( "stlr %w1, %0" : "=Q" ( *lock ) : "r" ( 0u ) : "memory" );
    __asm volatile ( "sev" ::: "memory" );
}

void uxPortTaskLock( void )
{
    smp_lock_acquire( &task_lock_held );
}

void uxPortTaskUnlock( void )
{
    smp_lock_release( &task_lock_held );
}

void uxPortISRLock( void )
{
    smp_lock_acquire( &isr_lock_held );
}

void uxPortISRUnlock( void )
{
    smp_lock_release( &isr_lock_held );
}

/*-----------------------------------------------------------*/

/*
 * See header file for description. The stack layout is the single-core
 * port's, and the two slots below the initial pstate (critical nesting,
 * FPU context) are kept so the asm restore path is shared verbatim - in SMP
 * the nesting slot is never read back.
 */
StackType_t * pxPortInitialiseStack( StackType_t * pxTopOfStack,
                                     TaskFunction_t pxCode,
                                     void * pvParameters )
{
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
    *pxTopOfStack = ( StackType_t ) 0x00;         /* R30 - procedure call link register. */

    pxTopOfStack--;
    *pxTopOfStack = portINITIAL_PSTATE;

    pxTopOfStack--;
    *pxTopOfStack = ( StackType_t ) pxCode; /* Exception return address. */

    {
        /* The task will start with a critical nesting count of 0 as
        * interrupts are enabled. (Slot kept for layout; SMP keeps the real
        * count in the TCB.) */
        pxTopOfStack--;
        *pxTopOfStack = portNO_CRITICAL_NESTING;

        /* The task will start without a floating point context.  A task that
        * uses the floating point hardware must call vPortTaskUsesFPU() before
        * executing any floating point instructions. */
        pxTopOfStack--;
        *pxTopOfStack = portNO_FLOATING_POINT_CONTEXT;
    }

    return pxTopOfStack;
}
/*-----------------------------------------------------------*/

static void uxPortYieldSGIHandler( void )
{
    /* Runs above the API-call priority and therefore must not (and does
     * not) call a kernel API: pending the switch on this core is the whole
     * job. The actual switch happens on IRQ exit, in the asm. */
    portEND_SWITCHING_ISR( pdTRUE );
}

static void uxPortInstallYieldSGIHandler( void )
{
    /* The SGI's priority and enable are armed with the rest of the per-core
     * GIC bring-up (board_gicv3_secondary_init, both cores); only the
     * handler is the port's business. */
    ( void ) IRQ_SetHandler( ( IRQn_ID_t ) portYIELD_SGI_INTID,
                             uxPortYieldSGIHandler );
}

/*-----------------------------------------------------------*/

BaseType_t xPortStartScheduler( void )
{
    uint32_t ulAPSR;

    __asm volatile ( "MRS %0, CurrentEL" : "=r" ( ulAPSR ) );

    ulAPSR &= portAPSR_MODE_BITS_MASK;

    /* Every core runs this at EL1: the boot core fell through startup.S,
     * secondaries through smp_secondary.S - both descend to EL1 first. */
    configASSERT( ulAPSR == portEL1 );

    if( ulAPSR == portEL1 )
    {
        /* Interrupts are turned off in the CPU itself to ensure a tick or
         * yield SGI does not execute while the scheduler is being started.
         * They are turned back on when the first task starts executing. */
        portDISABLE_INTERRUPTS();

        if( __atomic_load_n( &uxPortSchedulerRunning,
                             __ATOMIC_ACQUIRE ) == ( uint64_t ) pdFALSE )
        {
            /* Boot core: this core's own redistributor/CPU interface/yield
             * SGI arming. Idempotent over the boot path's earlier
             * board_gicv3_init().
             *
             * The secondaries are NOT released here. Bring-up rounds 6-16
             * showed that releasing cores before the scheduler runs - three
             * booting cores plus firmware console traffic inside the same
             * pre-tick silence - freezes the machine in ways that are pure
             * timing races and leave no evidence. Instead the scheduler
             * comes up on this core alone, and a low-priority application
             * task releases the secondaries afterwards (smp_boot task in
             * app/main.c): a straggling core then costs one core, not the
             * boot, and every stage is observable through the live shell. */
            board_gicv3_secondary_init();
            uxPortInstallYieldSGIHandler();

            /* One tick source, on this core only; the other cores are
             * driven through xYieldPendings and the yield SGI. */
            configSETUP_TICK_INTERRUPT();

            __atomic_store_n( &uxPortSchedulerRunning,
                              ( uint64_t ) pdTRUE, __ATOMIC_RELEASE );
            __asm volatile ( "dsb sy" ::: "memory" );
            __asm volatile ( "sev" ::: "memory" );
        }
        else
        {
            /* Secondary core: uxPortSecondaryMain already completed the
             * per-core GIC bring-up and reported in; only the handler is
             * left to attach. */
            uxPortInstallYieldSGIHandler();
        }

        /* Start the first task executing (this core's idle task - the
         * kernel pointed pxCurrentTCBs[core] at it in vTaskStartScheduler).
         * Does not return. */
        vPortRestoreTaskContext();
    }

    return 0;
}
/*-----------------------------------------------------------*/

/* Secondary-core C entry, called from smp_secondary.S. Never returns: the
 * tail of xPortStartScheduler erets into this core's idle task.
 *
 * The whole secondary path is SILENT until the scheduler runs: console
 * output here overlaps OP-TEE's I/TC release messages on the same UART,
 * and that concurrency wedged the transmitter mid-line during bring-up
 * (rounds 6-10 - output stopped half-sentence, system mute). A secondary
 * proves its life through its report-in flag, which the boot core's
 * "smp: N/4 cores up" anchor vouches for. */
void uxPortSecondaryMain( void )
{
    /* Own redistributor (WAKER handshake only), CPU interface, yield SGI
     * priority and enable - the per-core mirror of what the boot core ran.
     * Done BEFORE reporting in, so the boot core's bounded wait covers GIC
     * readiness and not just "the core is executing". */
    board_gicv3_secondary_init();
    board_smp_mark_core_up( board_smp_core_id() );

    /* Plain spin, no WFE: the boot core's SEV can land before this loop's
     * first WFE executes, and a secondary has no tick or other interrupt to
     * break a lost-event sleep. The wait is over in microseconds once the
     * boot core finishes arming the tick. */
    while( __atomic_load_n( &uxPortSchedulerRunning,
                            __ATOMIC_ACQUIRE ) == ( uint64_t ) pdFALSE )
    {
        __asm volatile ( "yield" ::: "memory" );
    }

    ( void ) xPortStartScheduler();

    /* Unreachable. */
    for( ; ; )
    {
        __asm volatile ( "wfe" ::: "memory" );
    }
}
/*-----------------------------------------------------------*/

void vPortEndScheduler( void )
{
    /* Not implemented in ports where there is nothing to return to.
     * Artificially force an assert. */
    configASSERT( ( volatile void * ) NULL );
}
/*-----------------------------------------------------------*/

void FreeRTOS_Tick_Handler( void )
{
    const BaseType_t xCoreID = portGET_CORE_ID();

    /* Must be the lowest possible priority. */
    {
        uint64_t ullRunningInterruptPriority;
        /* s3_0_c12_c11_3 is ICC_RPR_EL1. */
        __asm volatile ( "MRS %0, s3_0_c12_c11_3" : "=r" ( ullRunningInterruptPriority ) );
        configASSERT( ullRunningInterruptPriority == ( portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT ) );
    }

    /* Interrupts should not be enabled before this point. */
    {
        uint32_t ulMaskBits;

        __asm volatile ( "MRS %0, DAIF" : "=r" ( ulMaskBits )::"memory" );
        configASSERT( ( ulMaskBits & portDAIF_I ) != 0 );
    }

    /* Rearm the level-triggered timer first: the line only drops when the
     * period is reloaded, and it must not sit asserted through the locked
     * section below. */
    configCLEAR_TICK_INTERRUPT();

    /* The kernel does not lock inside xTaskIncrementTick - taking the ISR
     * lock around it is the port's contract. Interrupts stay fully masked
     * for the whole handler (no mid-handler re-enable like the single-core
     * port), so a nested ISR can never attempt to take this lock on this
     * core while it is held. Other cores' FromISR callers block on the lock
     * for the (few) instructions the increment takes. */
    uxPortISRLock();
    {
        /* Increment the RTOS tick. A true return pends the switch on THIS
         * core; other cores are reached through the kernel's prvYieldCore,
         * which sends them the yield SGI. */
        if( xTaskIncrementTick() != pdFALSE )
        {
            ullPortYieldRequired[ xCoreID ] = ( uint64_t ) pdTRUE;
        }
    }
    uxPortISRUnlock();
}
/*-----------------------------------------------------------*/

#if ( configUSE_TASK_FPU_SUPPORT != 2 )

void vPortTaskUsesFPU( void )
{
    /* A task is registering the fact that it needs an FPU context. Set the
     * FPU flag of the core running the caller; it is saved with (and
     * restored per) that core's task context. */
    ullPortTaskHasFPUContext[ portGET_CORE_ID() ] = pdTRUE;
}

#endif /* configUSE_TASK_FPU_SUPPORT */
/*-----------------------------------------------------------*/

void vPortClearInterruptMask( UBaseType_t uxNewMaskValue )
{
    if( uxNewMaskValue == pdFALSE )
    {
        portPMR_UNMASK_ALL();
    }
}
/*-----------------------------------------------------------*/

UBaseType_t uxPortSetInterruptMask( void )
{
    uint32_t ulReturn;
    uint64_t ullPMRValue;

    /* Interrupt in the CPU must be turned off while the ICCPMR is being
     * updated. */
    portDISABLE_INTERRUPTS();
    /* s3_0_c4_c6_0 is ICC_PMR_EL1. */
    __asm volatile ( "MRS %0, s3_0_c4_c6_0" : "=r" ( ullPMRValue ) );

    if( ullPMRValue == ( configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT ) )
    {
        /* Interrupts were already masked. */
        ulReturn = pdTRUE;
    }
    else
    {
        ulReturn = pdFALSE;
        /* s3_0_c4_c6_0 is ICC_PMR_EL1. */
        __asm volatile ( "MSR s3_0_c4_c6_0, %0      \n"
                         "DSB SY                    \n"
                         "ISB SY                    \n"
                         ::"r" ( configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT ) : "memory" );
    }

    portENABLE_INTERRUPTS();

    return ulReturn;
}
/*-----------------------------------------------------------*/

#if ( configASSERT_DEFINED == 1 )

    void vPortValidateInterruptPriority( void )
    {
        /* The following assertion will fail if a service routine (ISR) for
         * an interrupt that has been assigned a priority above
         * configMAX_SYSCALL_INTERRUPT_PRIORITY calls an ISR safe FreeRTOS API
         * function. See the vendored single-core port for the full
         * rationale - unchanged here. */
        uint64_t ullRunningInterruptPriority;
        /* s3_0_c12_c11_3 is ICC_RPR_EL1. */
        __asm volatile ( "MRS %0, s3_0_c12_c11_3" : "=r" ( ullRunningInterruptPriority ) );

        configASSERT( ullRunningInterruptPriority >= ( configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT ) );
    }

#endif /* configASSERT_DEFINED */
/*-----------------------------------------------------------*/

void vApplicationFPUSafeIRQHandler( uint32_t ulICCIAR )
{
    ( void ) ulICCIAR;
    configASSERT( ( volatile void * ) NULL );
}
