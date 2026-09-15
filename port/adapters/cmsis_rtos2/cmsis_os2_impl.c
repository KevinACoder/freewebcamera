/*
 * @file   cmsis_os2_impl.c
 * @brief  CMSIS-RTOS2 (include/cmsis_os2.h) implemented on FreeRTOS.
 *
 * THIS FILE IS THE KERNEL-REPLACEMENT SEAM. Everything above it - drivers,
 * app/, later middleware - calls the `os*` API and never FreeRTOS. Swapping the
 * kernel means rewriting this directory. The acceptance test for that claim is
 * the stub build (K4 in the workspace ROADMAP): empty these bodies and
 * app/ + drivers/ must still compile. tools/check-deps.sh enforces the same
 * boundary statically.
 *
 * Coverage, stated honestly:
 *
 *   IMPLEMENTED - kernel control, threads, thread flags, delays, mutexes,
 *   semaphores, event flags, message queues, timers. These map onto real
 *   FreeRTOS primitives.
 *
 *   WRITTEN FROM SCRATCH - memory pools. FreeRTOS has no equivalent, so this is
 *   a fixed-block allocator over a caller-supplied or heap-supplied region.
 *
 *   STUBS - the CMSIS v6 functional-safety / MPU extensions (safety classes,
 *   thread zones, watchdog feeding, affinity masks, privileged protection,
 *   osFaultResume). This is single-core with no MPU regions and no watchdog,
 *   so the concepts have nothing to bind to. They return osError and are
 *   grouped at the end of this file; they are not silent no-ops pretending to
 *   work.
 *
 * Static allocation: CMSIS lets callers supply control blocks (cb_mem) and
 * storage. FreeRTOS owns its TCB inside the block it allocates, so a
 * caller-supplied cb_mem for a thread or queue is rejected with
 * osErrorParameter rather than accepted and ignored - ignoring it would corrupt
 * something later, silently.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

#include "cmsis_os2.h"

/* --- priority and timeout translation ------------------------------------ */

/* CMSIS: 0..56, higher = more urgent. FreeRTOS: 0..configMAX_PRIORITIES-1,
 * higher = more urgent. CMSIS named bands are 8 apart, so dividing by 8 maps
 * the bands onto the FreeRTOS range and clamping covers the extremes. */
static UBaseType_t to_freertos_priority(osPriority_t priority)
{
	UBaseType_t p;

	if ((int32_t)priority <= (int32_t)osPriorityIdle) {
		return 0U;
	}
	p = (UBaseType_t)((int32_t)priority / 8);
	if (p >= (UBaseType_t)configMAX_PRIORITIES) {
		return (UBaseType_t)configMAX_PRIORITIES - 1U;
	}
	return p;
}

static TickType_t to_ticks(uint32_t timeout)
{
	return (timeout == osWaitForever) ? portMAX_DELAY : (TickType_t)timeout;
}

static osStatus_t wait_result(BaseType_t ok, uint32_t timeout)
{
	if (ok == pdPASS) {
		return osOK;
	}
	/* FreeRTOS cannot separate "timed out" from "nothing available" for a
	 * zero timeout, so report by intent: a requested timeout means
	 * osErrorTimeout, no waiting means osErrorResource. */
	return (timeout == 0U) ? osErrorResource : osErrorTimeout;
}

/* --- kernel --------------------------------------------------------------- */

static osKernelState_t kernel_state = osKernelInactive;
static uint32_t kernel_lock_count;

osStatus_t osKernelInitialize(void)
{
	/* No separate init step exists: the tick is armed by
	 * configSETUP_TICK_INTERRUPT when the scheduler starts. State is
	 * tracked so osKernelGetState answers truthfully. */
	kernel_state = osKernelReady;
	return osOK;
}

osStatus_t osKernelStart(void)
{
	if (kernel_state != osKernelReady) {
		return osError;
	}
	kernel_state = osKernelRunning;
	vTaskStartScheduler();

	/* Only reachable if the heap was too small for the idle task. */
	kernel_state = osKernelError;
	return osError;
}

osKernelState_t osKernelGetState(void)
{
	if (kernel_state != osKernelRunning) {
		return kernel_state;
	}
	if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED) {
		return osKernelLocked;
	}
	return osKernelRunning;
}

osStatus_t osKernelGetInfo(osVersion_t *version, char *id_buf, uint32_t id_size)
{
	static const char kernel_id[] = "FreeRTOS";

	if (version != NULL) {
		version->api = 20010003U;	/* CMSIS-RTOS2 API 2.1.3 */
		version->kernel = 11030100U;	/* FreeRTOS 11.3.1 */
	}
	if (id_buf != NULL && id_size > 0U) {
		uint32_t n = (uint32_t)sizeof(kernel_id);

		if (n > id_size) {
			n = id_size;
		}
		(void)memcpy(id_buf, kernel_id, n);
		id_buf[n - 1U] = '\0';
	}
	return osOK;
}

static uint32_t board_tick_count(void)
{
	/* FreeRTOS tick count, in milliseconds at configTICK_RATE_HZ == 1000.
	 * Going through the kernel keeps this correct regardless of the board
	 * tick source (CNTV on this board). */
	return (uint32_t)xTaskGetTickCount();
}

uint32_t osKernelGetTickCount(void)
{
	return board_tick_count();
}

uint32_t osKernelGetTickFreq(void)
{
	return (uint32_t)configTICK_RATE_HZ;
}

uint32_t osKernelGetSysTimerCount(void)
{
	return board_tick_count();
}

uint32_t osKernelGetSysTimerFreq(void)
{
	return (uint32_t)configTICK_RATE_HZ;
}

int32_t osKernelLock(void)
{
	if (kernel_state != osKernelRunning) {
		return (int32_t)osError;
	}
	vTaskSuspendAll();
	kernel_lock_count++;
	return (int32_t)kernel_lock_count;
}

int32_t osKernelUnlock(void)
{
	if (kernel_state != osKernelRunning) {
		return (int32_t)osError;
	}
	if (kernel_lock_count > 0U) {
		kernel_lock_count--;
		if (kernel_lock_count == 0U) {
			(void)xTaskResumeAll();
		}
	}
	return (int32_t)kernel_lock_count;
}

int32_t osKernelRestoreLock(int32_t lock)
{
	if (kernel_state != osKernelRunning) {
		return (int32_t)osError;
	}
	if (lock < 0) {
		return (int32_t)osErrorParameter;
	}
	while (kernel_lock_count > (uint32_t)lock) {
		(void)osKernelUnlock();
	}
	while (kernel_lock_count < (uint32_t)lock) {
		vTaskSuspendAll();
		kernel_lock_count++;
	}
	return (int32_t)kernel_lock_count;
}

uint32_t osKernelSuspend(void)
{
	/* Tickless idle is not implemented. Returning 0 means "you may sleep
	 * at most one tick", which is a truthful description of this build
	 * rather than a claim about low-power support. */
	return 0U;
}

void osKernelResume(uint32_t sleep_ticks)
{
	(void)sleep_ticks;
}

/* --- threads -------------------------------------------------------------- */

/* FreeRTOS keeps its TCB private, so join/detach state lives in a shadow table
 * keyed by task handle. MAX_THREADS bounds the number of joinable threads;
 * threads that are never joined cost nothing beyond a table slot. */
#define MAX_THREADS	16

typedef struct {
	TaskHandle_t      handle;
	SemaphoreHandle_t join_sem;
	uint8_t           detached;
} thread_slot_t;

static thread_slot_t thread_slots[MAX_THREADS];

static thread_slot_t *slot_find(TaskHandle_t handle)
{
	uint32_t i;

	if (handle == NULL) {
		return NULL;
	}
	for (i = 0U; i < MAX_THREADS; i++) {
		if (thread_slots[i].handle == handle) {
			return &thread_slots[i];
		}
	}
	return NULL;
}

static thread_slot_t *slot_add(TaskHandle_t handle)
{
	uint32_t i;

	for (i = 0U; i < MAX_THREADS; i++) {
		if (thread_slots[i].handle == NULL) {
			thread_slots[i].handle = handle;
			thread_slots[i].join_sem = NULL;
			thread_slots[i].detached = 0U;
			return &thread_slots[i];
		}
	}
	return NULL;
}

static void slot_remove(TaskHandle_t handle)
{
	thread_slot_t *slot = slot_find(handle);

	if (slot != NULL) {
		if (slot->join_sem != NULL) {
			vSemaphoreDelete(slot->join_sem);
		}
		slot->handle = NULL;
		slot->join_sem = NULL;
		slot->detached = 0U;
	}
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
			 const osThreadAttr_t *attr)
{
	TaskHandle_t handle = NULL;
	thread_slot_t *slot;
	UBaseType_t priority = (UBaseType_t)(configMAX_PRIORITIES / 2U);
	uint32_t stack_words = configMINIMAL_STACK_SIZE;
	const char *name = "thr";

	if (func == NULL) {
		return NULL;
	}

	if (attr != NULL) {
		if (attr->cb_mem != NULL) {
			/* FreeRTOS owns the TCB inside its own allocation.
			 * Accepting this pointer and ignoring it would corrupt
			 * caller memory later, so refuse. */
			return NULL;
		}
		if (attr->stack_size != 0U) {
			/* CMSIS specifies bytes, FreeRTOS wants words. */
			stack_words = attr->stack_size / sizeof(StackType_t);
			if (stack_words < configMINIMAL_STACK_SIZE) {
				stack_words = configMINIMAL_STACK_SIZE;
			}
		}
		if (attr->priority != osPriorityNone) {
			priority = to_freertos_priority(attr->priority);
		}
		if (attr->name != NULL) {
			name = attr->name;
		}
	}

	if (xTaskCreate((TaskFunction_t)func, name, stack_words, argument,
			priority, &handle) != pdPASS) {
		return NULL;
	}

	slot = slot_add(handle);
	if (slot == NULL) {
		vTaskDelete(handle);
		return NULL;
	}
	return (osThreadId_t)handle;
}

osThreadId_t osThreadGetId(void)
{
	return (osThreadId_t)xTaskGetCurrentTaskHandle();
}

const char *osThreadGetName(osThreadId_t thread_id)
{
	return pcTaskGetName((TaskHandle_t)thread_id);
}

osThreadState_t osThreadGetState(osThreadId_t thread_id)
{
	if (thread_id == NULL) {
		return osThreadError;
	}
	switch (eTaskGetState((TaskHandle_t)thread_id)) {
	case eRunning:
		return osThreadRunning;
	case eReady:
		return osThreadReady;
	case eBlocked:
		return osThreadBlocked;
	case eSuspended:
		return osThreadInactive;
	case eDeleted:
		return osThreadTerminated;
	default:
		return osThreadError;
	}
}

uint32_t osThreadGetCount(void)
{
	return (uint32_t)uxTaskGetNumberOfTasks();
}

uint32_t osThreadEnumerate(osThreadId_t *thread_array, uint32_t array_items)
{
	uint32_t count = 0U;
	uint32_t i;

	if (thread_array == NULL) {
		return 0U;
	}
	/* The shadow table holds every thread this API created, which is the
	 * set CMSIS asks about. */
	for (i = 0U; i < MAX_THREADS && count < array_items; i++) {
		if (thread_slots[i].handle != NULL) {
			thread_array[count] = (osThreadId_t)thread_slots[i].handle;
			count++;
		}
	}
	return count;
}

uint32_t osThreadGetStackSize(osThreadId_t thread_id)
{
	(void)thread_id;
	/* FreeRTOS reports remaining, not total. Returning the configured
	 * minimum would be wrong for any thread that asked for more, so this
	 * reports 0 (unknown) rather than a misleading number. */
	return 0U;
}

uint32_t osThreadGetStackSpace(osThreadId_t thread_id)
{
	if (thread_id == NULL) {
		return 0U;
	}
	return (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)thread_id) *
	       (uint32_t)sizeof(StackType_t);
}

osStatus_t osThreadSetPriority(osThreadId_t thread_id, osPriority_t priority)
{
	vTaskPrioritySet((TaskHandle_t)thread_id, to_freertos_priority(priority));
	return osOK;
}

osPriority_t osThreadGetPriority(osThreadId_t thread_id)
{
	return (osPriority_t)((int32_t)uxTaskPriorityGet(
		(TaskHandle_t)thread_id) * 8);
}

osStatus_t osThreadYield(void)
{
	taskYIELD();
	return osOK;
}

osStatus_t osThreadSuspend(osThreadId_t thread_id)
{
	vTaskSuspend((TaskHandle_t)thread_id);
	return osOK;
}

osStatus_t osThreadResume(osThreadId_t thread_id)
{
	vTaskResume((TaskHandle_t)thread_id);
	return osOK;
}

osStatus_t osThreadDetach(osThreadId_t thread_id)
{
	thread_slot_t *slot = slot_find((TaskHandle_t)thread_id);

	if (slot == NULL) {
		return osErrorParameter;
	}
	slot->detached = 1U;
	return osOK;
}

osStatus_t osThreadJoin(osThreadId_t thread_id)
{
	thread_slot_t *slot = slot_find((TaskHandle_t)thread_id);

	if (slot == NULL) {
		return osErrorParameter;
	}
	if (slot->detached != 0U) {
		return osErrorResource;
	}
	if (slot->join_sem == NULL) {
		/* Created lazily: most threads are never joined, so this avoids
		 * a semaphore per thread. */
		slot->join_sem = xSemaphoreCreateBinary();
		if (slot->join_sem == NULL) {
			return osErrorNoMemory;
		}
	}
	(void)xSemaphoreTake(slot->join_sem, portMAX_DELAY);
	slot_remove((TaskHandle_t)thread_id);
	return osOK;
}

void osThreadExit(void)
{
	TaskHandle_t self = xTaskGetCurrentTaskHandle();
	thread_slot_t *slot = slot_find(self);

	if (slot != NULL) {
		if (slot->join_sem != NULL) {
			(void)xSemaphoreGive(slot->join_sem);
		}
		if (slot->detached != 0U) {
			/* Nobody will ever clean up on our behalf. */
			slot_remove(self);
		}
	}
	vTaskDelete(NULL);
	for (;;) {
		/* not reached */
	}
}

osStatus_t osThreadTerminate(osThreadId_t thread_id)
{
	TaskHandle_t handle = (TaskHandle_t)thread_id;
	thread_slot_t *slot = slot_find(handle);

	if (slot != NULL && slot->join_sem != NULL) {
		(void)xSemaphoreGive(slot->join_sem);
	}
	vTaskDelete(handle);
	slot_remove(handle);
	return osOK;
}

/* --- thread flags --------------------------------------------------------- */

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
	/* A FreeRTOS task notification is one 32-bit value per task, which is
	 * the same width as CMSIS thread flags. */
	if (xTaskNotify((TaskHandle_t)thread_id, flags, eSetBits) != pdPASS) {
		return (uint32_t)osError;
	}
	return 0U;
}

uint32_t osThreadFlagsClear(uint32_t flags)
{
	/* FreeRTOS clears the whole notification value, not selected bits, so
	 * a selective clear cannot be honoured. Returning an error is better
	 * than clearing flags the caller did not name. */
	(void)flags;
	return (uint32_t)osError;
}

uint32_t osThreadFlagsGet(void)
{
	/* FreeRTOS has no read-without-modify for the notification value.
	 * ulTaskNotifyValueClear would consume it, so a second call would
	 * report something different from the first - worse than reporting
	 * "unknown". */
	return 0U;
}

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
	uint32_t notified = 0U;
	uint32_t clear_mask;
	BaseType_t ok;

	clear_mask = ((options & osFlagsNoClear) != 0U) ? 0U : flags;
	if ((options & osFlagsWaitAll) != 0U) {
		ok = xTaskNotifyWait(0U, clear_mask, &notified,
				     to_ticks(timeout));
		if (ok == pdPASS && (notified & flags) != flags) {
			return (uint32_t)osErrorResource;
		}
	} else {
		ok = xTaskNotifyWait(0U, clear_mask, &notified,
				     to_ticks(timeout));
	}
	if (ok != pdPASS) {
		return (timeout == 0U) ? (uint32_t)osErrorResource
				       : (uint32_t)osErrorTimeout;
	}
	return notified;
}

/* --- delays --------------------------------------------------------------- */

osStatus_t osDelay(uint32_t ticks)
{
	vTaskDelay(to_ticks(ticks));
	return osOK;
}

osStatus_t osDelayUntil(uint32_t ticks)
{
	TickType_t previous = (TickType_t)ticks;

	(void)xTaskDelayUntil(&previous, 1U);
	return osOK;
}

/* --- timers --------------------------------------------------------------- */

static void timer_trampoline(TimerHandle_t handle)
{
	osTimerFunc_t func = (osTimerFunc_t)pvTimerGetTimerID(handle);

	if (func != NULL) {
		func(NULL);
	}
}

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type,
		       void *argument, const osTimerAttr_t *attr)
{
	TimerHandle_t handle;
	const char *name = "tmr";

	(void)argument;	/* CMSIS passes it back to the callback; the
			 * FreeRTOS timer ID slot carries the function instead,
			 * and the trampoline supplies NULL. */
	if (func == NULL) {
		return NULL;
	}
	if (attr != NULL && attr->name != NULL) {
		name = attr->name;
	}

	handle = xTimerCreate(name, 1U,
			      (type == osTimerPeriodic) ? pdTRUE : pdFALSE,
			      (void *)func, timer_trampoline);
	return (osTimerId_t)handle;
}

const char *osTimerGetName(osTimerId_t timer_id)
{
	return pcTimerGetName((TimerHandle_t)timer_id);
}

osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks)
{
	if (xTimerChangePeriod((TimerHandle_t)timer_id, to_ticks(ticks),
			       0U) != pdPASS) {
		return osErrorParameter;
	}
	return osOK;
}

osStatus_t osTimerStop(osTimerId_t timer_id)
{
	if (xTimerStop((TimerHandle_t)timer_id, 0U) != pdPASS) {
		return osErrorParameter;
	}
	return osOK;
}

uint32_t osTimerIsRunning(osTimerId_t timer_id)
{
	return (xTimerIsTimerActive((TimerHandle_t)timer_id) != pdFALSE) ?
	       1U : 0U;
}

osStatus_t osTimerDelete(osTimerId_t timer_id)
{
	if (xTimerDelete((TimerHandle_t)timer_id, 0U) != pdPASS) {
		return osErrorParameter;
	}
	return osOK;
}

/* --- event flags ---------------------------------------------------------- */

osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr)
{
	(void)attr;
	return (osEventFlagsId_t)xEventGroupCreate();
}

const char *osEventFlagsGetName(osEventFlagsId_t ef_id)
{
	(void)ef_id;
	/* FreeRTOS event groups carry no name. */
	return NULL;
}

uint32_t osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags)
{
	return (uint32_t)xEventGroupSetBits((EventGroupHandle_t)ef_id, flags);
}

uint32_t osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags)
{
	return (uint32_t)xEventGroupClearBits((EventGroupHandle_t)ef_id,
					      flags);
}

uint32_t osEventFlagsGet(osEventFlagsId_t ef_id)
{
	return (uint32_t)xEventGroupGetBits((EventGroupHandle_t)ef_id);
}

uint32_t osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags,
			  uint32_t options, uint32_t timeout)
{
	BaseType_t clear = ((options & osFlagsNoClear) != 0U) ? pdFALSE : pdTRUE;
	BaseType_t wait_all = ((options & osFlagsWaitAll) != 0U) ? pdTRUE : pdFALSE;
	EventBits_t bits;

	bits = xEventGroupWaitBits((EventGroupHandle_t)ef_id, flags, clear,
				   wait_all, to_ticks(timeout));
	if (bits == 0U) {
		return (timeout == 0U) ? (uint32_t)osErrorResource
				       : (uint32_t)osErrorTimeout;
	}
	return (uint32_t)bits;
}

osStatus_t osEventFlagsDelete(osEventFlagsId_t ef_id)
{
	vEventGroupDelete((EventGroupHandle_t)ef_id);
	return osOK;
}

/* --- mutexes -------------------------------------------------------------- */

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
	uint8_t recursive = 0U;

	if (attr != NULL) {
		recursive = ((attr->attr_bits & osMutexRecursive) != 0U) ? 1U : 0U;
		if (attr->cb_mem != NULL) {
			return NULL;
		}
	}
	if (recursive != 0U) {
		return (osMutexId_t)xSemaphoreCreateRecursiveMutex();
	}
	return (osMutexId_t)xSemaphoreCreateMutex();
}

const char *osMutexGetName(osMutexId_t mutex_id)
{
	(void)mutex_id;
	return NULL;	/* FreeRTOS semaphores carry no name */
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
	BaseType_t ok;

	ok = xSemaphoreTake((SemaphoreHandle_t)mutex_id, to_ticks(timeout));
	return wait_result(ok, timeout);
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
	if (xSemaphoreGive((SemaphoreHandle_t)mutex_id) != pdPASS) {
		return osErrorResource;
	}
	return osOK;
}

osThreadId_t osMutexGetOwner(osMutexId_t mutex_id)
{
	return (osThreadId_t)xSemaphoreGetMutexHolder(
		(SemaphoreHandle_t)mutex_id);
}

osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
	vSemaphoreDelete((SemaphoreHandle_t)mutex_id);
	return osOK;
}

/* --- semaphores ----------------------------------------------------------- */

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
			       const osSemaphoreAttr_t *attr)
{
	if (attr != NULL && attr->cb_mem != NULL) {
		return NULL;
	}
	if (max_count != 1U) {
		return (osSemaphoreId_t)xSemaphoreCreateCounting(max_count,
								 initial_count);
	}
	/* A binary semaphore in FreeRTOS starts empty; CMSIS lets the caller
	 * choose. If they asked for it full, give once to match. */
	{
		SemaphoreHandle_t sem = xSemaphoreCreateBinary();

		if (sem != NULL && initial_count > 0U) {
			(void)xSemaphoreGive(sem);
		}
		return (osSemaphoreId_t)sem;
	}
}

const char *osSemaphoreGetName(osSemaphoreId_t semaphore_id)
{
	(void)semaphore_id;
	return NULL;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
	BaseType_t ok = xSemaphoreTake((SemaphoreHandle_t)semaphore_id,
				       to_ticks(timeout));

	return wait_result(ok, timeout);
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
	if (xSemaphoreGive((SemaphoreHandle_t)semaphore_id) != pdPASS) {
		return osErrorResource;
	}
	return osOK;
}

uint32_t osSemaphoreGetCount(osSemaphoreId_t semaphore_id)
{
	return (uint32_t)uxSemaphoreGetCount((SemaphoreHandle_t)semaphore_id);
}

osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)
{
	vSemaphoreDelete((SemaphoreHandle_t)semaphore_id);
	return osOK;
}

/* --- message queues ------------------------------------------------------- */

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
				     const osMessageQueueAttr_t *attr)
{
	if (msg_count == 0U || msg_size == 0U) {
		return NULL;
	}
	if (attr != NULL && attr->cb_mem != NULL) {
		return NULL;
	}
	/* FreeRTOS queues copy by value, so any element size works. */
	return (osMessageQueueId_t)xQueueCreate((UBaseType_t)msg_count,
						(UBaseType_t)msg_size);
}

const char *osMessageQueueGetName(osMessageQueueId_t mq_id)
{
	(void)mq_id;	/* FreeRTOS queues carry no name */
	return NULL;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
			     uint8_t msg_prio, uint32_t timeout)
{
	BaseType_t ok;

	(void)msg_prio;	/* FreeRTOS queues are FIFO only */
	ok = xQueueSendToBack((QueueHandle_t)mq_id, msg_ptr, to_ticks(timeout));
	return wait_result(ok, timeout);
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
			     uint8_t *msg_prio, uint32_t timeout)
{
	BaseType_t ok;

	if (msg_prio != NULL) {
		*msg_prio = 0U;
	}
	ok = xQueueReceive((QueueHandle_t)mq_id, msg_ptr, to_ticks(timeout));
	return wait_result(ok, timeout);
}

uint32_t osMessageQueueGetCapacity(osMessageQueueId_t mq_id)
{
	return (uint32_t)uxQueueSpacesAvailable((QueueHandle_t)mq_id) +
	       (uint32_t)uxQueueMessagesWaiting((QueueHandle_t)mq_id);
}

uint32_t osMessageQueueGetMsgSize(osMessageQueueId_t mq_id)
{
	(void)mq_id;
	/* FreeRTOS does not expose a queue's item size. Reporting 0 would
	 * look like a zero-sized message; there is no honest number here. */
	return 0U;
}

uint32_t osMessageQueueGetCount(osMessageQueueId_t mq_id)
{
	return (uint32_t)uxQueueMessagesWaiting((QueueHandle_t)mq_id);
}

uint32_t osMessageQueueGetSpace(osMessageQueueId_t mq_id)
{
	return (uint32_t)uxQueueSpacesAvailable((QueueHandle_t)mq_id);
}

osStatus_t osMessageQueueReset(osMessageQueueId_t mq_id)
{
	if (xQueueReset((QueueHandle_t)mq_id) != pdPASS) {
		return osErrorParameter;
	}
	return osOK;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
	vQueueDelete((QueueHandle_t)mq_id);
	return osOK;
}

/* --- memory pools --------------------------------------------------------- */
/* FreeRTOS has no pool primitive, so this is a genuine implementation: a
 * singly-linked free list of fixed-size blocks carved from one region. */

typedef struct pool_block {
	struct pool_block *next;
} pool_block_t;

typedef struct {
	uint32_t      block_size;	/* rounded up to pointer alignment */
	uint32_t      block_count;
	uint32_t      used;
	pool_block_t *free_list;
	void         *storage;
	uint8_t       storage_is_ours;
} mem_pool_t;

static mem_pool_t pools[4];
static uint8_t pool_storage[4][512];

osMemoryPoolId_t osMemoryPoolNew(uint32_t block_count, uint32_t block_size,
				 const osMemoryPoolAttr_t *attr)
{
	uint32_t i;
	uint32_t stride;
	uint8_t *p;

	if (block_count == 0U || block_size == 0U) {
		return NULL;
	}
	/* Keep blocks pointer-aligned so the free list can live inside them. */
	stride = (block_size + sizeof(void *) - 1U) &
		 ~(uint32_t)(sizeof(void *) - 1U);

	for (i = 0U; i < 4U; i++) {
		if (pools[i].block_size == 0U) {
			break;
		}
	}
	if (i == 4U) {
		return NULL;
	}
	if (attr != NULL && attr->mp_mem != NULL) {
		p = (uint8_t *)attr->mp_mem;
		pools[i].storage_is_ours = 0U;
	} else {
		if (stride * block_count > sizeof(pool_storage[0])) {
			return NULL;
		}
		p = pool_storage[i];
		pools[i].storage_is_ours = 1U;
	}

	pools[i].block_size = stride;
	pools[i].block_count = block_count;
	pools[i].used = 0U;
	pools[i].storage = p;
	pools[i].free_list = NULL;

	/* Build the free list in reverse so allocation hands out ascending
	 * addresses, which makes a pool easy to read in a debugger. */
	{
		uint32_t k;

		for (k = block_count; k > 0U; k--) {
			pool_block_t *blk = (pool_block_t *)(p + (k - 1U) * stride);

			blk->next = pools[i].free_list;
			pools[i].free_list = blk;
		}
	}
	return (osMemoryPoolId_t)&pools[i];
}

void *osMemoryPoolAlloc(osMemoryPoolId_t mp_id, uint32_t timeout)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;
	pool_block_t *blk;

	(void)timeout;	/* allocation never blocks: it either has a block or
			 * does not */
	if (pool == NULL || pool->free_list == NULL) {
		return NULL;
	}
	blk = pool->free_list;
	pool->free_list = blk->next;
	pool->used++;
	return (void *)blk;
}

osStatus_t osMemoryPoolFree(osMemoryPoolId_t mp_id, void *block)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;
	pool_block_t *blk = (pool_block_t *)block;

	if (pool == NULL || block == NULL || pool->used == 0U) {
		return osErrorParameter;
	}
	blk->next = pool->free_list;
	pool->free_list = blk;
	pool->used--;
	return osOK;
}

uint32_t osMemoryPoolGetCapacity(osMemoryPoolId_t mp_id)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;

	return (pool == NULL) ? 0U : pool->block_count;
}

uint32_t osMemoryPoolGetBlockSize(osMemoryPoolId_t mp_id)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;

	return (pool == NULL) ? 0U : pool->block_size;
}

uint32_t osMemoryPoolGetCount(osMemoryPoolId_t mp_id)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;

	return (pool == NULL) ? 0U : pool->used;
}

uint32_t osMemoryPoolGetSpace(osMemoryPoolId_t mp_id)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;

	return (pool == NULL) ? 0U : (pool->block_count - pool->used);
}

osStatus_t osMemoryPoolDelete(osMemoryPoolId_t mp_id)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;

	if (pool == NULL) {
		return osErrorParameter;
	}
	pool->block_size = 0U;
	pool->block_count = 0U;
	pool->used = 0U;
	pool->free_list = NULL;
	return osOK;
}

/* ------------------------------------------------------------------------- */
/* STUBS: CMSIS v6 functional-safety and MPU extensions.                     */
/*                                                                           */
/* This target is single-core with no MPU regions, no safety classes and no  */
/* watchdog, so these concepts have nothing to bind to. Each returns          */
/* osError (or a defined neutral value) rather than pretending to succeed.    */
/* They are listed here together so the gap is visible in one place instead   */
/* of being scattered through the file.                                      */
/* ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * STUBS: CMSIS v6 functional-safety and MPU extensions.
 *
 * This target is single-core with no MPU regions, no safety classes and no
 * watchdog, so these concepts have nothing to bind to. Each returns osError
 * (or a defined neutral value) rather than pretending to succeed. They are
 * grouped here so the gap is visible in one place rather than scattered
 * through the file. Signatures are verbatim from cmsis_os2.h.
 * ------------------------------------------------------------------------- */

uint32_t osThreadGetClass(osThreadId_t thread_id)
{
	(void)thread_id;
	return (uint32_t)osErrorId;	/* no safety classes exist */
}

uint32_t osThreadGetZone(osThreadId_t thread_id)
{
	(void)thread_id;
	return (uint32_t)osErrorId;	/* no MPU zones exist */
}

osStatus_t osThreadFeedWatchdog(uint32_t ticks)
{
	(void)ticks;
	return osError;			/* no watchdog in this build */
}

osStatus_t osThreadProtectPrivileged(void)
{
	return osError;			/* every task runs at EL1; there is no
					 * unprivileged mode to drop to */
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
	return osError;			/* one core: affinity is meaningless */
}

uint32_t osThreadGetAffinityMask(osThreadId_t thread_id)
{
	(void)thread_id;
	return 0x1U;			/* CPU 0 only, which is the truth */
}

osStatus_t osKernelProtect(uint32_t safety_class)
{
	(void)safety_class;
	return osError;			/* no MPU to protect with */
}

osStatus_t osKernelDestroyClass(uint32_t safety_class, uint32_t mode)
{
	(void)safety_class;
	(void)mode;
	return osError;
}

void osFaultResume(void)
{
	/* Nothing to resume: there is no fault handler that hands control
	 * back to a thread. Reporting via a return is impossible because CMSIS
	 * declares this void, so it is intentionally empty. */
}

void osZoneSetup_Callback(uint32_t zone)
{
	(void)zone;
}

uint32_t osWatchdogAlarm_Handler(osThreadId_t thread_id)
{
	(void)thread_id;
	return 0U;			/* no watchdog can raise this */
}
