/*
 * @file   fatfs_os.c
 * @brief  The OS-dependent functions FatFs asks the integrator for: volume
 *         mutexes and the name-buffer allocator.
 *
 * FatFs ships these as a sample (ffsystem.c) covering Windows, ITRON, FreeRTOS
 * and CMSIS-OS. That file is NOT vendored, for the same reason upstream's
 * ffconf.h is not: it is sample code whose whole purpose is to be replaced by
 * the integrator, and vendoring it would mean either editing a "byte-identical"
 * tree or leaving a second, unused implementation in the build. This is that
 * replacement, over the interfaces this project actually uses.
 *
 * The mutexes are CMSIS-RTOS2, as everything on this side of the kernel is.
 * They matter as soon as two contexts touch one volume - a shell command and
 * the network thread today, an httpd reading pages from the disk next - because
 * the symptom of a missing volume lock is not a deadlock but silently
 * interleaved window buffers, i.e. random file content.
 *
 * ff_memalloc/ff_memfree are only needed by the dynamic configurations
 * (FF_USE_LFN == 2/3) and by f_mkfs when the caller passes no work buffer. This
 * build uses the static LFN buffer (FF_USE_LFN == 1) and passes one, so they
 * exist to make those paths safe rather than because something calls them
 * today. They take the kernel heap, which is where the project's own dynamic
 * memory comes from.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>

#include "cmsis_os2.h"
#include "ff.h"

/* The integrator heap. Both kernel lines provide these names: FreeRTOS via
 * heap_4 over ucHeap, ThreadX via the adapter heap
 * (port/adapters/threadx/heap.c) - which is why this file stays shared
 * instead of growing a per-kernel twin. */
extern void *pvPortMalloc(size_t size);
extern void vPortFree(void *ptr);

static osMutexId_t volume_mutex[FF_VOLUMES];

int ff_mutex_create(int vol)
{
	if (vol < 0 || vol >= FF_VOLUMES) {
		return 0;
	}

	if (volume_mutex[vol] == NULL) {
		volume_mutex[vol] = osMutexNew(&(osMutexAttr_t){ .name = "fatfs" });
	}

	return (volume_mutex[vol] != NULL) ? 1 : 0;
}

void ff_mutex_delete(int vol)
{
	if (vol < 0 || vol >= FF_VOLUMES || volume_mutex[vol] == NULL) {
		return;
	}

	(void)osMutexDelete(volume_mutex[vol]);
	volume_mutex[vol] = NULL;
}

int ff_mutex_take(int vol)
{
	if (vol < 0 || vol >= FF_VOLUMES || volume_mutex[vol] == NULL) {
		return 0;
	}

	return (osMutexAcquire(volume_mutex[vol], FF_FS_TIMEOUT) == osOK) ? 1 : 0;
}

void ff_mutex_give(int vol)
{
	if (vol < 0 || vol >= FF_VOLUMES || volume_mutex[vol] == NULL) {
		return;
	}

	(void)osMutexRelease(volume_mutex[vol]);
}

void *ff_memalloc(UINT msize)
{
	return pvPortMalloc(msize);
}

void ff_memfree(void *mblock)
{
	vPortFree(mblock);
}