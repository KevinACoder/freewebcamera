/*
 * @file
 * @brief callout(9) shell over the host timer primitive.
 */

#ifndef _COMPAT_SYS_CALLOUT_H_
#define _COMPAT_SYS_CALLOUT_H_

#include <sys/cdefs.h>
#include "types.h"
#include <sys/mutex.h>

typedef void (*callout_fn_t)(void *);

/* embedded by driver softc; storage owned by the port */
struct callout {
	void *hc_timer;
	callout_fn_t hc_fn;
	void *hc_arg;
	int hc_pending;
	int hc_invoking;
	/* per-instance forensics for the 20260925 first-scan stall: which
	 * callout stopped being re-armed (scheds freeze) vs which callback
	 * stopped running (fires freeze) vs timer-thread blocking
	 * (long_fires).  hc_next threads every live callout onto the
	 * registry list "wlan callouts" walks. */
	unsigned hc_scheds;
	unsigned hc_fires;
	unsigned hc_long_fires;
	unsigned hc_last_fire_ms;
	/* deferral generation: bumped by stop/schedule; a fire that spent
	 * time in the callout-worker queue only runs if its captured
	 * generation is still current */
	unsigned hc_gen;
	unsigned hc_fire_gen;
	unsigned hc_stale_drops;
	struct callout *hc_next;
};
typedef struct callout callout_t;

#define CALLOUT_MPSAFE 0
#define CALLOUT_STOPPING 1

int callout_init(callout_t *c, int flags);
int callout_setfunc(callout_t *c, callout_fn_t fn, void *arg);
int callout_schedule(callout_t *c, int ticks);
int callout_stop(callout_t *c);
int callout_halt(callout_t *c, kmutex_t *lock);
void callout_destroy(callout_t *c);
bool callout_pending(callout_t *c);

/* NetBSD spells the arm-and-schedule pair as one macro over the setters */
#define callout_reset(c, to, fn, arg) do {				\
	(void) callout_setfunc((c), (fn), (arg));			\
	(void) callout_schedule((c), (to));				\
} while (0)

bool callout_pending(callout_t *c);
bool callout_invoking(callout_t *c);
int callout_ack(callout_t *c);

#endif /* _COMPAT_SYS_CALLOUT_H_ */
