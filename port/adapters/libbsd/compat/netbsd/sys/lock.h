/*
 * @file
 * @brief lock(9) shell for the imported sources.
 *
 * Upstream sys/lock.h ends at <machine/lock.h>, which is where the
 * aarch64 spinlock backoff hook lives; audio(4)'s track lock spins on
 * that macro.  The compat machine/lock.h provides it.
 *
 * This file shadows newlib's <sys/lock.h>, whose _LOCK_* vocabulary the
 * libc headers (sys/reent.h) need: chain to it first, then add the
 * NetBSD side.
 */

#ifndef _COMPAT_SYS_LOCK_H_
#define _COMPAT_SYS_LOCK_H_

#include <sys/cdefs.h>
#include_next <sys/lock.h>

#include <machine/lock.h>

#endif /* _COMPAT_SYS_LOCK_H_ */
