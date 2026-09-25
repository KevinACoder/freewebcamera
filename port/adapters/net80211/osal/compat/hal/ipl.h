/*
 * @file
 * @brief ipl(9) shim for the CMSIS-RTOS2 port.
 *
 * CMSIS-RTOS2 has no interrupt-mask API, so ipl_save/restore map onto
 * the kernel lock (scheduler locked). That is thread-grade protection:
 * an environment whose USB completions are posted from a true ISR must
 * route them through ISR-safe primitives (osSemaphoreRelease, thread
 * flags) or provide stronger wlan_ipl_save/wlan_ipl_restore and define
 * WLAN_PORT_CMSIS_IPL_EXTERN.
 *
 * @author zhugengyu
 * @date 22.09.2026
 */

#ifndef _COMPAT_HAL_IPL_H_
#define _COMPAT_HAL_IPL_H_

#include <stdint.h>
#include "cmsis_os2.h"

typedef int32_t ipl_t;

#define IPL_USB 0

#ifdef WLAN_PORT_CMSIS_IPL_EXTERN

extern ipl_t wlan_ipl_save(void);
extern void wlan_ipl_restore(ipl_t ipl);
#define ipl_save() wlan_ipl_save()
#define ipl_restore(ipl) wlan_ipl_restore((ipl))

#else

static inline ipl_t ipl_save(void) {
	return (ipl_t) osKernelLock();
}

static inline void ipl_restore(ipl_t ipl) {
	(void) osKernelRestoreLock((int32_t) ipl);
}

#endif

#endif /* _COMPAT_HAL_IPL_H_ */
