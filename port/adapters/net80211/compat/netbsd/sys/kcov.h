/*
 * @file
 * @brief kcov(9) coverage collection: compiled out - the uhub explore
 * path brackets itself with remote sections nobody collects here.
 */

#ifndef _COMPAT_SYS_KCOV_H_
#define _COMPAT_SYS_KCOV_H_

#include <sys/cdefs.h>

#define KCOV_REMOTE_COMMON	0
#define KCOV_REMOTE_VHCI	0x7000
#define KCOV_REMOTE_VHCI_ID(bus, port)	(((bus) << 16) | (port))

struct lwp;

static inline void kcov_remote_enter(int id, uint64_t sid) {
	(void) id; (void) sid;
}
static inline void kcov_remote_leave(int id, uint64_t sid) {
	(void) id; (void) sid;
}

#endif /* _COMPAT_SYS_KCOV_H_ */
