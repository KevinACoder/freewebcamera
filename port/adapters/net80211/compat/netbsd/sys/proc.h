/*
 * @file
 * @brief process context shell: single context.
 */

#ifndef _COMPAT_SYS_PROC_H_
#define _COMPAT_SYS_PROC_H_

#include <sys/cdefs.h>
#include <sys/kthread.h>
#include "cmsis_os2.h"

struct vmspace {
	int vs_dummy;
};

struct proc {
	struct vmspace *p_vmspace;
};

struct lwp {
	struct proc *l_proc;
};

typedef struct proc proc_t;

extern struct proc proc0_holder;

/* the proc lock: one global no-op (single context carrier) */
struct kmutex;
struct kmutex *proc_lock(struct proc *p);
#define curproc ((struct proc *)NULL)

/* the "lwp" identity is the CMSIS thread handle: the kthread backend
 * (osal) hands out osThread handles, so identity comparisons between
 * curlwp and a created worker actually work */
#define curlwp ((lwp_t) osThreadGetId())

/* tsleep priority: the value NetBSD's proc.h carries; the osal tsleep
 * ignores it */
#define PZERO 22

#endif /* _COMPAT_SYS_PROC_H_ */
