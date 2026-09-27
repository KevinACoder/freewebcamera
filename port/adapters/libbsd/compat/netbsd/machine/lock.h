/*
 * @file
 * @brief lock(4) shell: the aarch64 spinlock backoff the audio(4) track
 * lock spins with.
 *
 * Upstream's sys/arch/aarch64/include/lock.h defines the hook only under
 * _HARDKERNEL, which this build does not claim, so audio_track_lock_enter's
 * busy-wait would reference an undefined macro.  The instruction it names
 * is the architectural spin hint; the port's track lock is held for
 * bounded, short sections (one buffer block at most), so yielding in the
 * spin is exactly what the hook is for.
 */

#ifndef _COMPAT_MACHINE_LOCK_H_
#define _COMPAT_MACHINE_LOCK_H_

#ifndef SPINLOCK_BACKOFF_HOOK
#define SPINLOCK_BACKOFF_HOOK	__asm__ volatile("yield" ::: "memory")
#endif

#endif /* _COMPAT_MACHINE_LOCK_H_ */
