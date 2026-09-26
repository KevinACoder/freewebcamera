/*
 * @file   sdmmc_osa.c
 * @brief  The SDMMC_OSA* layer of the vendored fsl_sdmmc stack, implemented
 *         on CMSIS-RTOS2.
 *
 * This is the standalone line's osa/fsl_sdmmc_osa.c with exactly two
 * mechanical substitutions (decision D27): the SDK OSA calls behind it are
 * this project's CMSIS mapping (shadow/fsl_os_abstraction.h), and the DMA
 * -region memory calls are the kernel heap (the buffers it hands out are
 * word-access staging areas, not hardware queues). The event semantics -
 * semaphore-counted flag word, one post per set - are the vendored
 * SDMMC_OSA_POLLING_EVENT_BY_SEMPHORE default, kept as-is.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <string.h>
#include <stdint.h>

#include "fsl_sdmmc_osa.h"

/* The integrator heap, same two-kernel story as fatfs_os.c: heap_4 on the
 * FreeRTOS line, port/adapters/threadx/heap.c on the ThreadX line. */
extern void *pvPortMalloc(size_t size);
extern void vPortFree(void *ptr);

/* --- init ------------------------------------------------------------------- */

void SDMMC_OSAInit(void)
{
	/* intentional empty, matching the standalone implementation */
}

/* --- events (flag word + counting semaphore, the vendored default) ---------- */

status_t SDMMC_OSAEventCreate(void *eventHandle)
{
	if (eventHandle == NULL) {
		return kStatus_Fail;
	}

	(void)OSA_SemaphoreCreate(&(((sdmmc_osa_event_t *)eventHandle)->handle), 0U);

	return kStatus_Success;
}

status_t SDMMC_OSAEventWait(void *eventHandle, uint32_t eventType,
			    uint32_t timeoutMilliseconds, uint32_t *event,
			    uint32_t flags)
{
	if ((eventHandle == NULL) || (event == NULL)) {
		return kStatus_Fail;
	}

	osa_status_t status = KOSA_StatusError;

	while (true) {
		status = OSA_SemaphoreWait(&(((sdmmc_osa_event_t *)eventHandle)->handle),
					   timeoutMilliseconds);
		if (KOSA_StatusTimeout == status) {
			break;
		}

		if (KOSA_StatusSuccess == status) {
			(void)SDMMC_OSAEventGet(eventHandle, eventType, event);
			if (flags & SDMMC_OSA_EVENT_FLAG_AND) {
				if (*event == eventType) {
					return kStatus_Success;
				}
			} else {
				if ((*event & eventType) != 0U) {
					return kStatus_Success;
				}
			}
		}
	}

	return kStatus_Fail;
}

status_t SDMMC_OSAEventSet(void *eventHandle, uint32_t eventType)
{
	if (eventHandle == NULL) {
		return kStatus_Fail;
	}

	OSA_SR_ALLOC();
	OSA_ENTER_CRITICAL();
	((sdmmc_osa_event_t *)eventHandle)->eventFlag |= eventType;
	OSA_EXIT_CRITICAL();

	(void)OSA_SemaphorePost(&(((sdmmc_osa_event_t *)eventHandle)->handle));

	return kStatus_Success;
}

status_t SDMMC_OSAEventGet(void *eventHandle, uint32_t eventType, uint32_t *flag)
{
	if ((eventHandle == NULL) || (flag == NULL)) {
		return kStatus_Fail;
	}

	*flag = ((sdmmc_osa_event_t *)eventHandle)->eventFlag;

	return kStatus_Success;
}

status_t SDMMC_OSAEventClear(void *eventHandle, uint32_t eventType)
{
	if (eventHandle == NULL) {
		return kStatus_Fail;
	}

	OSA_SR_ALLOC();
	OSA_ENTER_CRITICAL();
	((sdmmc_osa_event_t *)eventHandle)->eventFlag &= ~eventType;
	OSA_EXIT_CRITICAL();

	return kStatus_Success;
}

status_t SDMMC_OSAEventDestroy(void *eventHandle)
{
	if (eventHandle == NULL) {
		return kStatus_Fail;
	}

	(void)OSA_SemaphoreDestroy(&(((sdmmc_osa_event_t *)eventHandle)->handle));

	return kStatus_Success;
}

/* --- mutex ------------------------------------------------------------------- */

status_t SDMMC_OSAMutexCreate(void *mutexHandle)
{
	if (mutexHandle == NULL) {
		return kStatus_Fail;
	}

	(void)OSA_MutexCreate(&((sdmmc_osa_mutex_t *)mutexHandle)->handle);

	return kStatus_Success;
}

status_t SDMMC_OSAMutexLock(void *mutexHandle, uint32_t millisec)
{
	if (mutexHandle == NULL) {
		return kStatus_Fail;
	}

	(void)OSA_MutexLock(&((sdmmc_osa_mutex_t *)mutexHandle)->handle, millisec);

	return kStatus_Success;
}

status_t SDMMC_OSAMutexUnlock(void *mutexHandle)
{
	if (mutexHandle == NULL) {
		return kStatus_Fail;
	}

	(void)OSA_MutexUnlock(&((sdmmc_osa_mutex_t *)mutexHandle)->handle);

	return kStatus_Success;
}

status_t SDMMC_OSAMutexDestroy(void *mutexHandle)
{
	if (mutexHandle == NULL) {
		return kStatus_Fail;
	}

	(void)OSA_MutexDestroy(&((sdmmc_osa_mutex_t *)mutexHandle)->handle);

	return kStatus_Success;
}

/* --- delays ------------------------------------------------------------------- */

void SDMMC_OSADelay(uint32_t milliseconds)
{
	OSA_TimeDelay(milliseconds);
}

uint32_t SDMMC_OSADelayUs(uint32_t microseconds)
{
	uint32_t milliseconds = microseconds / 1000U +
				((microseconds % 1000U) == 0U ? 0U : 1U);

	OSA_TimeDelay(milliseconds);

	return milliseconds * 1000U;
}

/* --- memory (the DMA-region heap of the original, over the kernel heap) ------ */

void *SDMMC_OSAMemoryAllocate(uint32_t length)
{
	return pvPortMalloc(length);
}

void *SDMMC_OSAMemoryAlignedAllocate(uint32_t length, uint32_t align)
{
	/* heap_4 hands out 8-aligned blocks; for anything larger, over-
	 * allocate and keep the original pointer in front of the payload. */
	void *raw;
	uintptr_t aligned;

	if (align <= 8U) {
		return pvPortMalloc(length);
	}

	raw = pvPortMalloc(length + align);
	if (raw == NULL) {
		return NULL;
	}

	aligned = ((uintptr_t)raw + align) & ~(uintptr_t)(align - 1U);
	((void **)(void *)aligned)[-1] = raw;

	return (void *)aligned;
}

void SDMMC_OSAMemoryFree(void *p)
{
	if (p == NULL) {
		return;
	}

	/* aligned blocks carry the raw pointer one slot in front; the check
	 * cannot distinguish the two cases, so aligned allocations must go
	 * through SDMMC_OSAMemoryAlignedFree below */
	vPortFree(p);
}

void SDMMC_OSAMemoryAlignedFree(void *p)
{
	if (p == NULL) {
		return;
	}

	vPortFree(((void **)(void *)p)[-1]);
}
