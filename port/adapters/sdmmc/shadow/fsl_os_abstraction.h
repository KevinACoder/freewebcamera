/*
 * @file   fsl_os_abstraction.h
 * @brief  Shadow of the NXP SDK OSA header: maps the OSA_* calls made by the
 *         vendored osa/fsl_sdmmc_osa.c onto CMSIS-RTOS2.
 *
 * The stack's OSA layer (third-party/sdmmc/osa/, byte-identical to upstream)
 * is written against NXP's fsl_os_abstraction.h. That header belongs to the
 * NXP SDK we do not vendor, so this shadow supplies the same names over
 * cmsis_os2.h - the same shape the project already uses for the FatFs
 * adapter (ff_mutex_* on osMutex*) and the general D13 rule that everything
 * above the port layer speaks CMSIS.
 *
 * Only the semaphore-flavoured event path is implemented
 * (SDMMC_OSA_POLLING_EVENT_BY_SEMPHORE is the vendored default): events are
 * an eventFlag word guarded by osKernelLock plus a counting semaphore, which
 * is what the vendored fsl_sdmmc_osa.c does with them. Handles are stored by
 * value inside the stack's own structs, which is what the
 * OSA_*_HANDLE_DEFINE expansion is for.
 *
 * Not an interface: nothing outside the sdmmc adapter includes this.
 */

#ifndef _FSL_OS_ABSTRACTION_H_
#define _FSL_OS_ABSTRACTION_H_

#include <stdint.h>

#include "cmsis_os2.h"
#include "fsl_common.h"

typedef enum _osa_status
{
	KOSA_StatusSuccess = 0U,
	KOSA_StatusError   = 1U,
	KOSA_StatusTimeout = 2U,
} osa_status_t;

#define osaWaitForever_c 0xFFFFFFFFU

/* Storage members: the stack embeds these in sdmmc_osa_event_t / mutex. */
#define OSA_SEMAPHORE_HANDLE_DEFINE(name) osSemaphoreId_t name
#define OSA_MUTEX_HANDLE_DEFINE(name)	  osMutexId_t name

static inline osa_status_t OSA_SemaphoreCreate(void *handle, uint32_t count)
{
	/* A counting semaphore: the event layer posts once per flag set. */
	*(osSemaphoreId_t *)handle = osSemaphoreNew(0xFFFFU, count, NULL);

	return (*(osSemaphoreId_t *)handle != NULL) ? KOSA_StatusSuccess : KOSA_StatusError;
}

static inline osa_status_t OSA_SemaphoreDestroy(void *handle)
{
	return (osSemaphoreDelete(*(osSemaphoreId_t *)handle) == osOK) ? KOSA_StatusSuccess : KOSA_StatusError;
}

static inline osa_status_t OSA_SemaphoreWait(void *handle, uint32_t millisec)
{
	return (osSemaphoreAcquire(*(osSemaphoreId_t *)handle, millisec) == osOK) ? KOSA_StatusSuccess : KOSA_StatusTimeout;
}

static inline osa_status_t OSA_SemaphorePost(void *handle)
{
	return (osSemaphoreRelease(*(osSemaphoreId_t *)handle) == osOK) ? KOSA_StatusSuccess : KOSA_StatusError;
}

static inline osa_status_t OSA_MutexCreate(void *handle)
{
	osMutexAttr_t attr = { 0 };

	attr.name      = "sdmmc";
	attr.attr_bits = osMutexPrioInherit;
	*(osMutexId_t *)handle = osMutexNew(&attr);

	return (*(osMutexId_t *)handle != NULL) ? KOSA_StatusSuccess : KOSA_StatusError;
}

static inline osa_status_t OSA_MutexDestroy(void *handle)
{
	return (osMutexDelete(*(osMutexId_t *)handle) == osOK) ? KOSA_StatusSuccess : KOSA_StatusError;
}

static inline osa_status_t OSA_MutexLock(void *handle, uint32_t millisec)
{
	return (osMutexAcquire(*(osMutexId_t *)handle, millisec) == osOK) ? KOSA_StatusSuccess : KOSA_StatusTimeout;
}

static inline osa_status_t OSA_MutexUnlock(void *handle)
{
	return (osMutexRelease(*(osMutexId_t *)handle) == osOK) ? KOSA_StatusSuccess : KOSA_StatusError;
}

/* Critical sections here only guard an eventFlag word in task context. */
#define OSA_SR_ALLOC()	  int osa_sr_
#define OSA_ENTER_CRITICAL() (osa_sr_ = (int)osKernelLock())
#define OSA_EXIT_CRITICAL()  ((void)osKernelRestoreLock(osa_sr_))

static inline void OSA_TimeDelay(uint32_t millisec)
{
	(void)osDelay(millisec);
}

#endif /* _FSL_OS_ABSTRACTION_H_ */
