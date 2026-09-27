/*
 * @file
 * @brief condvar(9) shell: the kcondvar_t storage and the cv_* calls
 *        live with the mutex shells in sys/mutex.h.
 */

#ifndef _COMPAT_SYS_CONDVAR_H_
#define _COMPAT_SYS_CONDVAR_H_

#include <sys/mutex.h>

int wlan_cv_signal(kcondvar_t *cv, unsigned n);

#define cv_signal(cv) wlan_cv_signal((cv), 1)
#define cv_broadcast(cv) wlan_cv_broadcast(cv)
#define cv_wait_sig(cv, mtx) wlan_cv_wait((cv), (mtx))
#define cv_timedwait_sig(cv, mtx, ticks) wlan_cv_timedwait((cv), (mtx), (ticks))

#endif /* _COMPAT_SYS_CONDVAR_H_ */
