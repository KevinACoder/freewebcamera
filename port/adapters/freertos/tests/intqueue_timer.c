/*
 * @file   intqueue_timer.c
 * @brief  Second interrupt source for the IntQueue stress test, in the shape
 *         of the official ThirdParty/Template/IntQueueTimer.c.
 *
 * The official template leaves the hardware half to the board: the tick hook
 * (test_runner.c) drives xFirstTimerHandler() every tick, and this file must
 * supply a SECOND, slower interrupt that runs IntQueueTestTimerHandler()
 * (which is xSecondTimerHandler() plus the yield).
 *
 * There is no spare timer PPI on this board to do that with: the virtual
 * timer's line (PPI27) pends but is never delivered to the boot core in SMP
 * mode - the reason the tick itself moved to CNTPNS/PPI30 (board_conf.h).
 * So the second source is a software-pended SPI instead:
 *
 *   - INTID BOARD_KTEST_INTQ_INTID, a spare SPI the main image uses for its
 *     GIC soft-trigger probe (that probe does not run in the ktest image).
 *     IRQ_SetPending/IRQ_ClearPending on a real SPI is board-proven there.
 *   - Priority BOARD_IRQ_PRIORITY_INTQ_TIMER: numerically between the
 *     FromISR ceiling and the tick, so it genuinely PREEMPTS the tick
 *     handler (the nesting the test exists to exercise) while every FromISR
 *     call it makes stays inside the port's validated band.
 *   - Pacing: a self-pended interrupt would re-enter at wire speed, so the
 *     handler re-arms itself only when it actually ran the test handler,
 *     gated on the ISR-visible tick count. One wasted entry per period is
 *     the whole cost; the test sees a steady ~50 Hz second source.
 *
 * Single-core note: the strict "task must be suspended" checks inside
 * IntQueue.c are active there (see the FREERTOS_PORT notes in that file),
 * and this source behaves identically at any core count.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "irq_ctrl.h"

#include "IntQueue.h"
#include "IntQueueTimer.h"

/* Rate of the second interrupt source. The test imposes no timing: it only
 * needs sustained traffic from two independent contexts. 50 Hz keeps the
 * queue ISR load well under the tick's. */
#define intqPERIOD_TICKS	pdMS_TO_TICKS( 20UL )

static TickType_t xLastFire;

/* The official template's contract: the board timer's ISR ends up here, and
 * this wraps the test's second queue-ISR half with the yield request. */
void IntQueueTestTimerHandler( void )
{
	portYIELD_FROM_ISR( xSecondTimerHandler() );
}

static void intqTimerIsr( void )
{
	TickType_t xNow = xTaskGetTickCountFromISR();

	IRQ_ClearPending( ( IRQn_ID_t ) BOARD_KTEST_INTQ_INTID );

	if( ( TickType_t ) ( xNow - xLastFire ) >= intqPERIOD_TICKS )
	{
		xLastFire = xNow;
		IntQueueTestTimerHandler();
		IRQ_SetPending( ( IRQn_ID_t ) BOARD_KTEST_INTQ_INTID );
	}
}

void vInitialiseTimerForIntQueueTest( void )
{
	/* Runs after the scheduler is up (IntQueue.c starts it from inside its
	 * own high-priority task), so the GIC and the dispatch path are live. */
	IRQ_SetHandler( ( IRQn_ID_t ) BOARD_KTEST_INTQ_INTID, intqTimerIsr );
	IRQ_SetPriority( ( IRQn_ID_t ) BOARD_KTEST_INTQ_INTID,
			 BOARD_IRQ_PRIORITY_INTQ_TIMER_RAW );
	IRQ_Enable( ( IRQn_ID_t ) BOARD_KTEST_INTQ_INTID );

	xLastFire = xTaskGetTickCount();
	IRQ_SetPending( ( IRQn_ID_t ) BOARD_KTEST_INTQ_INTID );
}
