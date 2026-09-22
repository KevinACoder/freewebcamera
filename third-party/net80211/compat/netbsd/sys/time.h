/*
 * @file
 * @brief time structs for the imported USB headers.
 */

#ifndef _COMPAT_SYS_TIME_H_
#define _COMPAT_SYS_TIME_H_

#include <sys/cdefs.h>
#include <sys/types.h>

/* The libc (newlib) defines the same structs unconditionally behind
 * its own _SYS__*_H_ guards, so either header may arrive first: reuse
 * the libc guard names here, whichever side defines first wins and
 * the layouts are identical. */
#ifndef _SYS__TIMESPEC_H_
struct timespec {
	long tv_sec;
	long tv_nsec;
};
#define _SYS__TIMESPEC_H_
#endif

#ifndef _SYS__TIMEVAL_H_
struct timeval {
	long tv_sec;
	long tv_usec;
};
#define _SYS__TIMEVAL_H_
#endif

#endif /* _COMPAT_SYS_TIME_H_ */
