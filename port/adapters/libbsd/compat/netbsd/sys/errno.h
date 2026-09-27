/*
 * @file
 * @brief Error numbers: chain to the toolchain's <sys/errno.h>
 * (include_next, the same discipline as cdefs.h) so newlib's own
 * errno.h finds its codes instead of this shadow's guard.
 */

#ifndef _COMPAT_SYS_ERRNO_H_
#define _COMPAT_SYS_ERRNO_H_

#include_next <sys/errno.h>

#endif /* _COMPAT_SYS_ERRNO_H_ */
