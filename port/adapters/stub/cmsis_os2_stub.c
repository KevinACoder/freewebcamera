/*
 * @file   cmsis_os2_stub.c
 * @brief  Null CMSIS-RTOS2 implementation, used to prove the kernel seam (K4).
 *
 * This file exists to answer one question: is CMSIS-RTOS2 a real boundary, or
 * a label we put on things? The way to tell is to remove the kernel entirely
 * and see whether the layers above still compile. If `app/` and `drivers/`
 * build against these stubs, then nothing above them depends on FreeRTOS - the
 * interface is load-bearing, and swapping the kernel is a matter of writing a
 * different file behind the same header.
 *
 * NOTHING HERE RUNS. The bodies are deliberately empty or return errors, and
 * this translation unit is never linked into an image. It is compiled, and the
 * layers above it are compiled against it, and that is the whole test.
 *
 * The prototypes are taken verbatim from include/cmsis_os2.h; if that header
 * grows a function, this file is expected to fail to link until it is covered,
 * which is the point.
 */

#include "cmsis_os2.h"

/* --- kernel --------------------------------------------------------------- */

osStatus_t osKernelInitialize(void) { return osError; }
osStatus_t osKernelStart(void) { return osError; }
uint32_t osKernelGetTickCount(void) { return 0U; }
uint32_t osKernelGetTickFreq(void) { return 0U; }
uint32_t osKernelGetSysTimerCount(void) { return 0U; }
uint32_t osKernelGetSysTimerFreq(void) { return 0U; }
osKernelState_t osKernelGetState(void) { return osKernelInactive; }
int32_t osKernelLock(void) { return 0; }
int32_t osKernelUnlock(void) { return 0; }
int32_t osKernelRestoreLock(int32_t lock) { (void)lock; return 0; }
uint32_t osKernelSuspend(void) { return 0U; }
void osKernelResume(uint32_t tick) { (void)tick; }

/* --- threads -------------------------------------------------------------- */

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
			 const osThreadAttr_t *attr)
{
	(void)func; (void)argument; (void)attr;
	return NULL;
}
const char *osThreadGetName(osThreadId_t thread_id)
{
	(void)thread_id;
	return NULL;
}
osThreadId_t osThreadGetId(void) { return NULL; }
osThreadState_t osThreadGetState(osThreadId_t thread_id)
{
	(void)thread_id;
	return osThreadError;
}
uint32_t osThreadGetStackSize(osThreadId_t thread_id)
{
	(void)thread_id;
	return 0U;
}
uint32_t osThreadGetStackSpace(osThreadId_t thread_id)
{
	(void)thread_id;
	return 0U;
}
osStatus_t osThreadSetPriority(osThreadId_t thread_id, osPriority_t priority)
{
	(void)thread_id; (void)priority;
	return osError;
}
osPriority_t osThreadGetPriority(osThreadId_t thread_id)
{
	(void)thread_id;
	return osPriorityError;
}
osStatus_t osThreadYield(void) { return osError; }
osStatus_t osThreadSuspend(osThreadId_t thread_id)
{
	(void)thread_id;
	return osError;
}
osStatus_t osThreadResume(osThreadId_t thread_id)
{
	(void)thread_id;
	return osError;
}
osStatus_t osThreadDetach(osThreadId_t thread_id)
{
	(void)thread_id;
	return osError;
}
osStatus_t osThreadJoin(osThreadId_t thread_id)
{
	(void)thread_id;
	return osError;
}
__NO_RETURN void osThreadExit(void) { for (;;) { } }
osStatus_t osThreadTerminate(osThreadId_t thread_id)
{
	(void)thread_id;
	return osError;
}
uint32_t osThreadGetCount(void) { return 0U; }
uint32_t osThreadEnumerate(osThreadId_t *thread_array, uint32_t array_items)
{
	(void)thread_array; (void)array_items;
	return 0U;
}

/* --- thread flags --------------------------------------------------------- */

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
	(void)thread_id; (void)flags;
	return (uint32_t)osError;
}
uint32_t osThreadFlagsClear(uint32_t flags)
{
	(void)flags;
	return (uint32_t)osError;
}
uint32_t osThreadFlagsGet(void) { return 0U; }
uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
	(void)flags; (void)options; (void)timeout;
	return (uint32_t)osError;
}

/* --- delays --------------------------------------------------------------- */

osStatus_t osDelay(uint32_t ticks) { (void)ticks; return osError; }
osStatus_t osDelayUntil(uint32_t ticks) { (void)ticks; return osError; }

/* --- timers --------------------------------------------------------------- */

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void *argument,
		       const osTimerAttr_t *attr)
{
	(void)func; (void)type; (void)argument; (void)attr;
	return NULL;
}
const char *osTimerGetName(osTimerId_t timer_id)
{
	(void)timer_id;
	return NULL;
}
osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks)
{
	(void)timer_id; (void)ticks;
	return osError;
}
osStatus_t osTimerStop(osTimerId_t timer_id)
{
	(void)timer_id;
	return osError;
}
uint32_t osTimerIsRunning(osTimerId_t timer_id)
{
	(void)timer_id;
	return 0U;
}
osStatus_t osTimerDelete(osTimerId_t timer_id)
{
	(void)timer_id;
	return osError;
}

/* --- event flags ---------------------------------------------------------- */

osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr)
{
	(void)attr;
	return NULL;
}
const char *osEventFlagsGetName(osEventFlagsId_t ef_id)
{
	(void)ef_id;
	return NULL;
}
uint32_t osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags)
{
	(void)ef_id; (void)flags;
	return (uint32_t)osError;
}
uint32_t osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags)
{
	(void)ef_id; (void)flags;
	return (uint32_t)osError;
}
uint32_t osEventFlagsGet(osEventFlagsId_t ef_id)
{
	(void)ef_id;
	return 0U;
}
uint32_t osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags,
			  uint32_t options, uint32_t timeout)
{
	(void)ef_id; (void)flags; (void)options; (void)timeout;
	return (uint32_t)osError;
}
osStatus_t osEventFlagsDelete(osEventFlagsId_t ef_id)
{
	(void)ef_id;
	return osError;
}

/* --- mutexes -------------------------------------------------------------- */

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
	(void)attr;
	return NULL;
}
const char *osMutexGetName(osMutexId_t mutex_id)
{
	(void)mutex_id;
	return NULL;
}
osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
	(void)mutex_id; (void)timeout;
	return osError;
}
osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
	(void)mutex_id;
	return osError;
}
osThreadId_t osMutexGetOwner(osMutexId_t mutex_id)
{
	(void)mutex_id;
	return NULL;
}
osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
	(void)mutex_id;
	return osError;
}

/* --- semaphores ----------------------------------------------------------- */

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
			       const osSemaphoreAttr_t *attr)
{
	(void)max_count; (void)initial_count; (void)attr;
	return NULL;
}
const char *osSemaphoreGetName(osSemaphoreId_t semaphore_id)
{
	(void)semaphore_id;
	return NULL;
}
osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
	(void)semaphore_id; (void)timeout;
	return osError;
}
osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
	(void)semaphore_id;
	return osError;
}
uint32_t osSemaphoreGetCount(osSemaphoreId_t semaphore_id)
{
	(void)semaphore_id;
	return 0U;
}
osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)
{
	(void)semaphore_id;
	return osError;
}

/* --- message queues ------------------------------------------------------- */

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
				     const osMessageQueueAttr_t *attr)
{
	(void)msg_count; (void)msg_size; (void)attr;
	return NULL;
}
const char *osMessageQueueGetName(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	return NULL;
}
osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
			     uint8_t msg_prio, uint32_t timeout)
{
	(void)mq_id; (void)msg_ptr; (void)msg_prio; (void)timeout;
	return osError;
}
osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
			     uint8_t *msg_prio, uint32_t timeout)
{
	(void)mq_id; (void)msg_ptr; (void)msg_prio; (void)timeout;
	return osError;
}
uint32_t osMessageQueueGetCapacity(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	return 0U;
}
uint32_t osMessageQueueGetMsgSize(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	return 0U;
}
uint32_t osMessageQueueGetCount(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	return 0U;
}
uint32_t osMessageQueueGetSpace(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	return 0U;
}
osStatus_t osMessageQueueReset(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	return osError;
}
osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	return osError;
}

/* --- memory pools --------------------------------------------------------- */

osMemoryPoolId_t osMemoryPoolNew(uint32_t block_count, uint32_t block_size,
				 const osMemoryPoolAttr_t *attr)
{
	(void)block_count; (void)block_size; (void)attr;
	return NULL;
}
const char *osMemoryPoolGetName(osMemoryPoolId_t mp_id)
{
	(void)mp_id;
	return NULL;
}
void *osMemoryPoolAlloc(osMemoryPoolId_t mp_id, uint32_t timeout)
{
	(void)mp_id; (void)timeout;
	return NULL;
}
osStatus_t osMemoryPoolFree(osMemoryPoolId_t mp_id, void *block)
{
	(void)mp_id; (void)block;
	return osError;
}
uint32_t osMemoryPoolGetCapacity(osMemoryPoolId_t mp_id)
{
	(void)mp_id;
	return 0U;
}
uint32_t osMemoryPoolGetBlockSize(osMemoryPoolId_t mp_id)
{
	(void)mp_id;
	return 0U;
}
uint32_t osMemoryPoolGetCount(osMemoryPoolId_t mp_id)
{
	(void)mp_id;
	return 0U;
}
uint32_t osMemoryPoolGetSpace(osMemoryPoolId_t mp_id)
{
	(void)mp_id;
	return 0U;
}
osStatus_t osMemoryPoolDelete(osMemoryPoolId_t mp_id)
{
	(void)mp_id;
	return osError;
}

/* --- v6 functional-safety extensions -------------------------------------
 *
 * Same policy and the same signatures as the real adapter: this is single-core
 * with no MPU regions and no watchdog, so report the error rather than pretend
 * to have honoured the request. Kept identical here so the stub exercises the
 * same API surface the real build does.
 * ------------------------------------------------------------------------- */

uint32_t osThreadGetClass(osThreadId_t thread_id)
{
	(void)thread_id;
	return (uint32_t)osErrorId;
}

uint32_t osThreadGetZone(osThreadId_t thread_id)
{
	(void)thread_id;
	return (uint32_t)osErrorId;
}

osStatus_t osThreadFeedWatchdog(uint32_t ticks)
{
	(void)ticks;
	return osError;
}

osStatus_t osThreadProtectPrivileged(void)
{
	return osError;
}

osStatus_t osThreadSuspendClass(uint32_t safety_class, uint32_t mode)
{
	(void)safety_class;
	(void)mode;
	return osError;
}

osStatus_t osThreadResumeClass(uint32_t safety_class, uint32_t mode)
{
	(void)safety_class;
	(void)mode;
	return osError;
}

osStatus_t osThreadTerminateZone(uint32_t zone)
{
	(void)zone;
	return osError;
}

osStatus_t osThreadSetAffinityMask(osThreadId_t thread_id, uint32_t affinity_mask)
{
	(void)thread_id;
	(void)affinity_mask;
	return osError;
}

uint32_t osThreadGetAffinityMask(osThreadId_t thread_id)
{
	(void)thread_id;
	return 0x1U;
}

osStatus_t osKernelProtect(uint32_t safety_class)
{
	(void)safety_class;
	return osError;
}

osStatus_t osKernelDestroyClass(uint32_t safety_class, uint32_t mode)
{
	(void)safety_class;
	(void)mode;
	return osError;
}

void osFaultResume(void)
{
}

void osZoneSetup_Callback(uint32_t zone)
{
	(void)zone;
}

uint32_t osWatchdogAlarm_Handler(osThreadId_t thread_id)
{
	(void)thread_id;
	return 0U;
}

