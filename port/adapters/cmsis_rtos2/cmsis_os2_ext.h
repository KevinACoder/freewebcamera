/*
 * @file   cmsis_os2_ext.h
 * @brief  Project extensions to CMSIS-RTOS2. NOT part of CMSIS.
 *
 * Why this file exists rather than additions to cmsis_os2.h:
 *
 * The vendored CMSIS header is kept byte-identical (that is gate K7 - the whole
 * point of vendoring is that it can be diffed against upstream), so extensions
 * cannot go there. Putting them in a separate header keeps the vendored file
 * untouched while still giving the extension one obvious home.
 *
 * ---------------------------------------------------------------------------
 * WHY AN ISR-SAFE SETTER IS NEEDED AT ALL
 *
 * CMSIS-RTOS2 deliberately has no ISR-safe flag setter. Its signal-from-
 * interrupt path is osTimer, osEventFlagsSet/osSemaphoreRelease (which are
 * ISR-safe) and the osMemoryPool/osMessageQueue equivalents. Those are all
 * fine when the interrupt is "something happened, wake a worker" - which is
 * most of the time, and is the pattern the shell uses (see below).
 *
 * The extension exists for the cases where routing through an EventFlags or
 * Semaphore object is the wrong shape:
 *
 *  - A driver whose ISR must report *which* bits fired to one specific task,
 *    with no shared object and no allocation at init time. A UART receiving
 *    into a ring buffer needs exactly this: the ISR says "there are bytes",
 *    the task needs nothing else, and giving the ISR a semaphore handle it
 *    must validate adds state to get wrong.
 *
 *  - Interrupt contexts where the wake-up must not be lost if the task is not
 *    yet blocked - thread flags latch, a semaphore does not unless it is
 *    counted.
 *
 * This is the same extension several RTOS vendors ship (`osThreadFlagsSetFromISR`
 * appears in vendor CMSIS-RTOS2 packages), so it is not an invention - but it is
 * also not in the standard, which is why it is fenced off here with a name that
 * says so.
 *
 * RULE FOR USE: an interrupt that calls this must run at or below
 * configMAX_API_CALL_INTERRUPT_PRIORITY, or the port's priority assertion fires.
 * BOARD_IRQ_PRIORITY_API_CALL_RAW is the value to configure.
 * ---------------------------------------------------------------------------
 */

#ifndef FREEWEBCAMERA_CMSIS_OS2_EXT_H
#define FREEWEBCAMERA_CMSIS_OS2_EXT_H

#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set a thread's flags from interrupt context.
 *
 * Returns the flags value after setting, or osError if the call was rejected
 * (e.g. it found the scheduler not running). The CMSIS "flags after setting"
 * convention is kept so this behaves like osThreadFlagsSet.
 *
 * Must be called from an interrupt, and not from a critical section that
 * already masks interrupts. */
uint32_t osThreadFlagsSetFromISR(osThreadId_t thread_id, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_CMSIS_OS2_EXT_H */
