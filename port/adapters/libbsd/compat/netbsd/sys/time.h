/*
 * @file
 * @brief time structs for the imported USB headers.
 *
 * The guard is upstream sys/time.h's own name (_SYS_TIME_H_) on purpose:
 * it must NOT collide with _COMPAT_SYS_TIME_H_, which is the guard of the
 * tree's compat/sys/time.h - that one still has to include its
 * time_types.h (struct timeval50) when videoio.h pulls it under _KERNEL,
 * and a shared guard silently swallowed the whole file (the "timestamp
 * has incomplete type" round).
 */

#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

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

/* the interval forms the compat time_types.h ABI references (upstream
 * sys/time.h field order) */
struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
};

struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};

#define TIMEVAL_TO_TIMESPEC(tv, ts) do { \
	(ts)->tv_sec = (tv)->tv_sec; \
	(ts)->tv_nsec = (long) ((tv)->tv_usec * 1000); \
} while (0)
#define TIMESPEC_TO_TIMEVAL(tv, ts) do { \
	(tv)->tv_sec = (ts)->tv_sec; \
	(tv)->tv_usec = (long) ((ts)->tv_nsec / 1000); \
} while (0)

#endif /* _SYS_TIME_H_ */
