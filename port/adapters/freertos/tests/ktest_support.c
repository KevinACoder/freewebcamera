/*
 * @file   ktest_support.c
 * @brief  Kernel-test image glue: boot banner + live suite status.
 *
 * kernel_tests_boot() replaces the main image's application bring-up: it
 * prints what is about to run and hands over to the official TestRunner's
 * vStartTests(), which creates the check task and every enabled suite's
 * tasks and then STARTS THE SCHEDULER itself (the official flow - this
 * image therefore never calls osKernelStart()).
 *
 * ktest_report() backs the interactive `ktest` shell command: one line per
 * enabled suite, running the same xAre*StillRunning check functions the
 * monitor task uses, so a console that can still accept input can ask "how
 * does it look right now" without waiting for the 5 s sweep line.
 *
 * Everything runs through board_log (spinlock-serialized, safe from any
 * core); test files never print on their own - the whole suite's console
 * output is the monitor task's one status line per 5 s, exactly like the
 * official template.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "tests_config.h"

#include "BlockQ.h"
#include "AbortDelay.h"
#include "blocktim.h"
#include "countsem.h"
#include "death.h"
#include "dynamic.h"
#include "EventGroupsDemo.h"
#include "GenQTest.h"
#include "integer.h"
#include "IntQueue.h"
#include "IntSemTest.h"
#include "PollQ.h"
#include "QPeek.h"
#include "QueueOverwrite.h"
#include "QueueSet.h"
#include "QueueSetPolling.h"
#include "recmutex.h"
#include "semtest.h"
#include "TaskNotify.h"
#include "TaskNotifyArray.h"
#include "TimerDemo.h"

/* Provided by test_runner.c (official vStartTests, starts the scheduler). */
extern void vStartTests( void );

void kernel_tests_boot( void )
{
	board_log( "ktest: FreeRTOS kernel test suite, %u core(s)",
		   ( unsigned int ) configNUMBER_OF_CORES );
	board_log( "ktest: suites- sem q cntsem recmutex genq pollq blockq" );
	board_log( "ktest:          qpeek dynamic death intmath notify notifyarr" );
	board_log( "ktest:          evgrp qowr qset qsetpoll blocktim abtdelay" );
	board_log( "ktest:          timers intsem intqueue" );
	board_log( "ktest: monitor line every 5s: 'No errors' or 'Error: <suite>'" );

	vStartTests();

	/* vStartTests ends in vTaskStartScheduler(); reaching here means the
	 * scheduler refused to start. */
	board_log( "fatal: vTaskStartScheduler returned" );
	taskDISABLE_INTERRUPTS();
	for( ; ; )
	{
		__asm__ __volatile__( "wfe" );
	}
}

/* One line per suite: "PASS" while the suite's tasks keep making progress,
 * "STALL" when a suite's loop counters stopped moving. Mirrors the monitor
 * task's checks so the command agrees with the periodic line. */
void ktest_report( void )
{
	TickType_t tick = xTaskGetTickCount();

	#define KTEST_LINE( name, call ) \
		board_log( "ktest: %-12s %s", name, \
			   ( ( call ) == pdTRUE ) ? "PASS" : "STALL" )

	#if ( configSTART_TASK_NOTIFY_TESTS == 1 )
	KTEST_LINE( "notify", xAreTaskNotificationTasksStillRunning() );
	#endif
	#if ( configSTART_TASK_NOTIFY_ARRAY_TESTS == 1 )
	KTEST_LINE( "notifyarr", xAreTaskNotificationArrayTasksStillRunning() );
	#endif
	#if ( configSTART_BLOCKING_QUEUE_TESTS == 1 )
	KTEST_LINE( "blockq", xAreBlockingQueuesStillRunning() );
	#endif
	#if ( configSTART_SEMAPHORE_TESTS == 1 )
	KTEST_LINE( "sem", xAreSemaphoreTasksStillRunning() );
	#endif
	#if ( configSTART_POLLED_QUEUE_TESTS == 1 )
	KTEST_LINE( "pollq", xArePollingQueuesStillRunning() );
	#endif
	#if ( configSTART_INTEGER_MATH_TESTS == 1 )
	KTEST_LINE( "intmath", xAreIntegerMathsTaskStillRunning() );
	#endif
	#if ( configSTART_GENERIC_QUEUE_TESTS == 1 )
	KTEST_LINE( "genq", xAreGenericQueueTasksStillRunning() );
	#endif
	#if ( configSTART_PEEK_QUEUE_TESTS == 1 )
	KTEST_LINE( "qpeek", xAreQueuePeekTasksStillRunning() );
	#endif
	#if ( configSTART_RECURSIVE_MUTEX_TESTS == 1 )
	KTEST_LINE( "recmutex", xAreRecursiveMutexTasksStillRunning() );
	#endif
	#if ( configSTART_COUNTING_SEMAPHORE_TESTS == 1 )
	KTEST_LINE( "cntsem", xAreCountingSemaphoreTasksStillRunning() );
	#endif
	#if ( configSTART_QUEUE_SET_TESTS == 1 )
	KTEST_LINE( "qset", xAreQueueSetTasksStillRunning() );
	#endif
	#if ( configSTART_QUEUE_OVERWRITE_TESTS == 1 )
	KTEST_LINE( "qowr", xIsQueueOverwriteTaskStillRunning() );
	#endif
	#if ( configSTART_EVENT_GROUP_TESTS == 1 )
	KTEST_LINE( "evgrp", xAreEventGroupTasksStillRunning() );
	#endif
	#if ( configSTART_INTERRUPT_SEMAPHORE_TESTS == 1 )
	KTEST_LINE( "intsem", xAreInterruptSemaphoreTasksStillRunning() );
	#endif
	#if ( configSTART_QUEUE_SET_POLLING_TESTS == 1 )
	KTEST_LINE( "qsetpoll", xAreQueueSetPollTasksStillRunning() );
	#endif
	#if ( configSTART_BLOCK_TIME_TESTS == 1 )
	KTEST_LINE( "blocktim", xAreBlockTimeTestTasksStillRunning() );
	#endif
	#if ( configSTART_ABORT_DELAY_TESTS == 1 )
	KTEST_LINE( "abtdelay", xAreAbortDelayTestTasksStillRunning() );
	#endif
	#if ( ( configSTART_TIMER_TESTS == 1 ) && ( configUSE_PREEMPTION != 0 ) )
	KTEST_LINE( "timers", xAreTimerDemoTasksStillRunning( pdMS_TO_TICKS( 5000UL ) ) );
	#endif
	#if ( configSTART_INTERRUPT_QUEUE_TESTS == 1 )
	KTEST_LINE( "intqueue", xAreIntQueueTasksStillRunning() );
	#endif
	#if ( configSTART_DELETE_SELF_TESTS == 1 )
	KTEST_LINE( "death", xIsCreateTaskStillRunning() );
	#endif

	#undef KTEST_LINE

	board_log( "ktest: tick=%u", ( unsigned int ) tick );
}
