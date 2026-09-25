/*
 * @file
 * @brief interrupt IPL shell; ports keep their own discipline.
 */

#ifndef _COMPAT_SYS_INTR_H_
#define _COMPAT_SYS_INTR_H_

#include <sys/cdefs.h>

#include <hal/ipl.h>

#ifndef IPL_NONE
#define IPL_NONE 0
#endif
#ifndef IPL_SOFTNET
#define IPL_SOFTNET 1
#endif
#ifndef IPL_SOFTSERIAL
#define IPL_SOFTSERIAL 1
#endif
#ifndef IPL_NET
#define IPL_NET 2
#endif
#ifndef IPL_USB
#define IPL_USB IPL_NET
#endif

/* the interrupt worker: the backend runs the handler on a thread */
#define SOFTINT_CLOCK		0
#define SOFTINT_BIO		1
#define SOFTINT_NET		2
#define SOFTINT_SERIAL		3
#define SOFTINT_USB		4
#define SOFTINT_MPSAFE		0x100
void *softint_establish(int flags, void (*func)(void *), void *arg);
void softint_schedule(void *sih);

ipl_t splraiseipl(ipl_t);
ipl_t splnet(void);
bool cpu_intr_p(void);
bool cpu_softintr_p(void);
ipl_t splsoftserial(void);
void splx(ipl_t);

#endif /* _COMPAT_SYS_INTR_H_ */
