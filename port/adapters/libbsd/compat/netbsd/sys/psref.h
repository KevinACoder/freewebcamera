/*
 * @file
 * @brief psref(9) shell: passive references on a single-context carrier.
 *
 * audio(4) uses psref to pin its softc while a file operation runs
 * (audio_sc_acquire_fromfile): psref_acquire records the target, release
 * drops it, and psref_target_destroy waits for extant references.  With
 * one execution context and non-preemptive teardown (detach is never
 * concurrent with a open/read here) the acquire/release pair only has to
 * record state; the destroy handshake is already satisfied by
 * construction.  Shapes follow upstream so the field accesses compile.
 */

#ifndef _COMPAT_SYS_PSREF_H_
#define _COMPAT_SYS_PSREF_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/queue.h>

struct cpu_info;
struct lwp;

struct psref;
struct psref_class;

struct psref_target {
	struct psref_class	*prt_class;
	bool			prt_draining;
};

struct psref {
	SLIST_ENTRY(psref)		psref_entry;
	void				*psref_debug;
	const struct psref_target	*psref_target;
	struct lwp			*psref_lwp;
	struct cpu_info			*psref_cpu;
};

void	psref_init(void);

struct psref_class *psref_class_create(const char *, int);
void	psref_class_destroy(struct psref_class *);

void	psref_target_init(struct psref_target *, struct psref_class *);
void	psref_target_destroy(struct psref_target *, struct psref_class *);

void	psref_acquire(struct psref *, const struct psref_target *,
	    struct psref_class *);
void	psref_release(struct psref *, const struct psref_target *,
	    struct psref_class *);
void	psref_copy(struct psref *, const struct psref *,
	    struct psref_class *);

bool	psref_held(const struct psref_target *, struct psref_class *);

#define PSREF_DEBUG_BARRIER()		__nothing
#define PSREF_DEBUG_INIT_LWP(l)		__nothing
#define PSREF_DEBUG_FILL_RETURN_ADDRESS(psref)	__nothing

#endif /* _COMPAT_SYS_PSREF_H_ */
