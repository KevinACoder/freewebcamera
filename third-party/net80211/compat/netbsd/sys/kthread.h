/*
 * @file
 * @brief kthread(9) shell: kthreads map onto the port worker threads
 *        (wlan_port_thread_create, the same primitives the usbdi shim
 *        workers use). The handle stored into *lwpp is the port thread.
 */

#ifndef _COMPAT_SYS_KTHREAD_H_
#define _COMPAT_SYS_KTHREAD_H_

#include <sys/types.h>

typedef void *lwp_t;
typedef int pri_t;

#ifndef PRI_NONE
#define PRI_NONE (-1)
#endif

struct cpu_info;

int kthread_create(pri_t pri, int flags, struct cpu_info *ci,
    void (*func)(void *), void *arg, lwp_t **lwpp, const char *fmt, ...);
void kthread_exit(int);

#endif /* _COMPAT_SYS_KTHREAD_H_ */
