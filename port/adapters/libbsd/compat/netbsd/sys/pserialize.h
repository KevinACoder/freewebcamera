/*
 * @file
 * @brief pserialize(9) shell: read sections need no quiescence here.
 *
 * audio(4) wraps its softc lookup in a pserialize read section and calls
 * pserialize_perform() before destroying the target, to wait for readers
 * that might still be inside.  The port has a single execution context and
 * no preemption of the audio softc teardown, so a read section is a no-op
 * and perform() has nothing to wait for.
 */

#ifndef _COMPAT_SYS_PSERIALIZE_H_
#define _COMPAT_SYS_PSERIALIZE_H_

#include <sys/cdefs.h>
#include <sys/types.h>

struct pserialize;
typedef struct pserialize *pserialize_t;
typedef int pserialize_read_critical_t;

pserialize_t pserialize_create(void);
void pserialize_destroy(pserialize_t);

pserialize_read_critical_t pserialize_read_enter(void);
void pserialize_read_exit(pserialize_read_critical_t);

void pserialize_perform(pserialize_t);

#endif /* _COMPAT_SYS_PSERIALIZE_H_ */
