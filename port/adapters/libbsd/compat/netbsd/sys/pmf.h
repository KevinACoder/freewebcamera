/*
 * @file
 * @brief pmf(9) shell: the no-op stubs live in sys/device.h next to the
 *        other config(9) shims.
 */

#ifndef _COMPAT_SYS_PMF_H_
#define _COMPAT_SYS_PMF_H_

#include <sys/device.h>

/* quality-of-service records: stored by the softcs, never evaluated */
typedef struct pmf_qual {
	int pq_dummy;
} pmf_qual_t;

typedef struct pmf_qual pmf_qual_common_t;

#endif /* _COMPAT_SYS_PMF_H_ */
