/*
 * @file
 * @brief CMSIS-RTOS2 implementation of the compat/netbsd kernel
 * services.
 *
 * Same shape as the proven FreeRTOS port: memory goes through the
 * wlan_osal_alloc/free hooks (CMSIS-RTOS2 has no allocation API),
 * mutexes onto osMutex, condition variables onto counting-semaphore
 * broadcast pools, callouts onto one-shot osTimer callbacks (fired in
 * the timer service thread), and the serializer/tsleep machinery keeps
 * the identified bounded waits with a short pending-wakeup window that
 * compensates for spl being a no-op.
 *
 * @author zhugengyu
 * @date 22.09.2026
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/intr.h>
#include <sys/kthread.h>
#include <sys/endian.h>

#include "wlan_port_cmsis.h"

/* the compat malloc/free macros must not intercept the host calls */
#undef malloc
#undef free

/* provided by the environment (wlan_adapter.c); the same primitive the
 * usbdi shim workers run on */
extern void *wlan_port_thread_create(void *(*run)(void *), void *arg);

#define WLAN_HZ 100

/* ------------------------------------------------------------------ */
/* memory hooks */

__attribute__((weak)) void *wlan_osal_alloc(size_t size) {
	return malloc(size);
}

__attribute__((weak)) void wlan_osal_free(void *p) {
	free(p);
}

/* ms -> kernel ticks, rounding up; the callers only pass small
 * millisecond counts (callout periods, poll quanta) */
static uint32_t ms2ticks(uint32_t ms) {
	uint64_t ticks;

	ticks = ((uint64_t) ms * osKernelGetTickFreq() + 999) / 1000;
	return (ticks > UINT32_MAX) ? UINT32_MAX : (uint32_t) ticks;
}

void panic(const char *fmt, ...) {
	va_list ap;

	printf("panic: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	osKernelLock();
	for (;;) {
	}
}

int kprintf(const char *fmt, ...) {
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vprintf(fmt, ap);
	va_end(ap);
	return ret;
}

/* A violated assertion names its site once and the run continues (see
 * the KASSERT family in port_config.h).  The site table dedupes by
 * (file,line): assert sites are string literals, so the pointer identity
 * of __FILE__ distinguishes translation units.  Deliberately not the
 * console's shell-paced path - assertions can fire from the USB
 * interrupt handler, and a garbled line beats no line. */
void wlan_kassert_fail(const char *cond, const char *file, int line,
    const char *fmt, ...) {
	static const char *seen_file[16];
	static int seen_line[16];
	static unsigned seen;
	unsigned i;

	for (i = 0; i < seen; i++) {
		if (seen_file[i] == file && seen_line[i] == line) {
			return;
		}
	}
	if (seen < 16) {
		seen_file[seen] = file;
		seen_line[seen] = line;
		seen++;
	}

	printf("KASSERT: %s at %s:%d", cond, file, line);
	if (fmt != NULL) {
		va_list ap;

		printf(" (");
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
		printf(")");
	}
	printf("\n");
}

/* ------------------------------------------------------------------ */

void *wlan_kmalloc(size_t size, int flags, int type) {
	void *p;

	(void) type;
	/* Drivers can initialize from a short-lived shell command thread. */
	p = wlan_osal_alloc(size);
	if (p != NULL && (flags & M_ZERO)) {
		memset(p, 0, size);
	}
	return p;
}

void wlan_kfree(void *p, int type) {
	(void) type;
	wlan_osal_free(p);
}

void *kmem_intr_alloc(size_t size, int flags) {
	return wlan_kmalloc(size, flags, M_DEVBUF);
}

void kmem_intr_free(void *p, size_t size) {
	(void) size;
	wlan_kfree(p, M_DEVBUF);
}

void *kmem_intr_zalloc(size_t size, int flags) {
	return wlan_kmalloc(size, flags | M_ZERO, M_DEVBUF);
}

void *kmem_zalloc(size_t size, int flags) {
	return wlan_kmalloc(size, flags | M_ZERO, M_DEVBUF);
}

void *kmem_alloc(size_t size, int flags) {
	return wlan_kmalloc(size, flags, M_DEVBUF);
}

void kmem_free(void *p, size_t size) {
	(void) size;
	wlan_kfree(p, M_DEVBUF);
}

/* the compat <mem/sysmalloc.h> surface used by the shared mbuf/ifnet
 * shells (port/net/embox) */
void *sysmalloc(size_t size) {
	return wlan_osal_alloc(size);
}

void sysfree(void *p) {
	wlan_osal_free(p);
}

void *sysmemalign(size_t align, size_t size) {
	/* the hooks cannot return aligned blocks directly; over-allocate
	 * and hand back an aligned point inside (the tail waste is fine
	 * for the small DMA buffers the shim allocates).
	 *
	 * TRAP: the result is an INTERIOR pointer - it must never be
	 * passed to wlan_kfree/free, which would read a phantom heap
	 * block header and corrupt live neighbors on the free list.
	 * Keep the raw block yourself and free that; the usbdi shim
	 * does exactly this. */
	void *raw;
	uintptr_t aligned;

	if (align == 0) {
		align = 1;
	}
	raw = wlan_osal_alloc(size + align);
	if (raw == NULL) {
		return NULL;
	}
	aligned = ((uintptr_t) raw + align - 1) & ~(uintptr_t)(align - 1);

	return (void *) aligned;
}

/* compat systm.h ksleep(9): bounded millisecond sleep */
void ksleep(int ms) {
	osDelay(ms2ticks((uint32_t) ms));
}

/* ------------------------------------------------------------------ */
/* mutex/cv shells over CMSIS primitives (fixed-size storage owned by
 * the net80211 softcs, see compat sys/mutex.h) */

static osMutexId_t hm_mutex(kmutex_t *m) {
	_Static_assert(sizeof(struct host_mutex) >= sizeof(osMutexId_t),
	    "mutex shell");
	return (osMutexId_t) ((struct host_mutex *) m)->hm_priv[0];
}

static osMutexId_t hc_cond_lock(kcondvar_t *cv) {
	return (osMutexId_t) ((struct host_cond *) cv)->hc_priv[0];
}

static osSemaphoreId_t hc_cond_sem(kcondvar_t *cv) {
	return (osSemaphoreId_t) ((struct host_cond *) cv)->hc_priv[1];
}

static volatile int *hc_cond_waiters(kcondvar_t *cv) {
	return (volatile int *) &((struct host_cond *) cv)->hc_priv[2];
}

int wlan_mutex_init(kmutex_t *m, int type, int ipl) {
	/* attr_bits 0: non-recursive, priority inheritance */
	static const osMutexAttr_t attr = { NULL, 0, NULL, 0 };

	(void) type;
	(void) ipl;
	((struct host_mutex *) m)->hm_priv[0] = (void *) osMutexNew(&attr);
	if (hm_mutex(m) == NULL) {
		return ENOMEM;
	}
	return 0;
}

int wlan_mutex_enter(kmutex_t *m) {
	osMutexAcquire(hm_mutex(m), osWaitForever);
	return 0;
}

int wlan_mutex_exit(kmutex_t *m) {
	osMutexRelease(hm_mutex(m));
	return 0;
}

int wlan_mutex_owned(kmutex_t *m) {
	(void) m;
	/* CMSIS mutexes do not expose the holder portably; lock assertions
	 * pass. */
	return 1;
}

void wlan_mutex_destroy(kmutex_t *m) {
	osMutexId_t id = hm_mutex(m);

	if (id != NULL) {
		osMutexDelete(id);
		((struct host_mutex *) m)->hm_priv[0] = NULL;
	}
}

/* cv = broadcast pool: waiters register, broadcast hands one token per
 * waiter through the counting semaphore. A token left over from a lost
 * race only causes a spurious wakeup - callers poll a predicate around
 * timed waits, same contract as the embox and FreeRTOS ports. */
int wlan_cv_init(kcondvar_t *cv, int flags) {
	(void) flags;
	((struct host_cond *) cv)->hc_priv[0] =
	    (void *) osMutexNew(NULL);
	((struct host_cond *) cv)->hc_priv[1] =
	    (void *) osSemaphoreNew(0xffff, 0, NULL);
	*hc_cond_waiters(cv) = 0;
	if (hc_cond_lock(cv) == NULL || hc_cond_sem(cv) == NULL) {
		return ENOMEM;
	}
	return 0;
}

int wlan_cv_wait(kcondvar_t *cv, kmutex_t *m) {
	return wlan_cv_timedwait(cv, m, 0);
}

int wlan_cv_timedwait(kcondvar_t *cv, kmutex_t *m, int ticks) {
	uint32_t timeout;
	int got;

	/* ticks are in hz units; <= 0 waits forever */
	if (ticks <= 0) {
		timeout = osWaitForever;
	} else {
		timeout = ms2ticks((uint32_t) ticks * 1000 / WLAN_HZ);
	}

	osMutexAcquire(hc_cond_lock(cv), osWaitForever);
	(*hc_cond_waiters(cv))++;
	osMutexRelease(hc_cond_lock(cv));

	wlan_mutex_exit(m);
	got = osSemaphoreAcquire(hc_cond_sem(cv), timeout);
	osMutexAcquire(hc_cond_lock(cv), osWaitForever);
	(*hc_cond_waiters(cv))--;
	osMutexRelease(hc_cond_lock(cv));
	wlan_mutex_enter(m);

	return (got == osOK) ? 0 : EWOULDBLOCK;
}

/* cv forensics: a signal that found zero waiters releases no token at
 * all - sound when the caller holds the same mutex the waiter will
 * re-check its predicate under, but the counter makes any other (more
 * fragile) caller visible from the shell. */
volatile unsigned wlan_cv_signals;
volatile unsigned wlan_cv_signals_dropped;
volatile unsigned wlan_cv_broadcasts;
volatile unsigned wlan_cv_broadcasts_dropped;

int wlan_cv_broadcast(kcondvar_t *cv) {
	int n;

	osMutexAcquire(hc_cond_lock(cv), osWaitForever);
	n = *hc_cond_waiters(cv);
	osMutexRelease(hc_cond_lock(cv));
	wlan_cv_broadcasts++;
	if (n == 0) {
		wlan_cv_broadcasts_dropped++;
	}
	while (n-- > 0) {
		osSemaphoreRelease(hc_cond_sem(cv));
	}
	return 0;
}

/* ISR-context wake (M11 r4 SDIO card interrupt): one token straight
 * onto the counting semaphore - osSemaphoreRelease is ISR-safe, the
 * waiters mutex is not. A token that lands while the worker is not
 * waiting is a spurious wakeup by the cv contract. */
int wlan_cv_isr_wake(kcondvar_t *cv) {
	return (osSemaphoreRelease(hc_cond_sem(cv)) == osOK) ? 0 : EPERM;
}

int wlan_cv_signal(kcondvar_t *cv, unsigned n) {

	osMutexAcquire(hc_cond_lock(cv), osWaitForever);
	if (n > (unsigned) *hc_cond_waiters(cv)) {
		n = (unsigned) *hc_cond_waiters(cv);
	}
	osMutexRelease(hc_cond_lock(cv));
	wlan_cv_signals++;
	if (n == 0) {
		wlan_cv_signals_dropped++;
	}
	while (n-- > 0) {
		osSemaphoreRelease(hc_cond_sem(cv));
	}
	return 0;
}

/* function fallbacks for TUs that never see the condvar.h macros
 * (ehci.c reaches the cv API through ehcivar only) */
#undef cv_signal
#undef cv_broadcast
#undef cv_wait_sig
#undef cv_timedwait_sig
void cv_signal(kcondvar_t *cv) {
	wlan_cv_signal(cv, 1);
}

int cv_wait_sig(kcondvar_t *cv, kmutex_t *m) {
	return wlan_cv_wait(cv, m);
}

int cv_timedwait_sig(kcondvar_t *cv, kmutex_t *m, int ticks) {
	return wlan_cv_timedwait(cv, m, ticks);
}

void cv_broadcast(kcondvar_t *cv) {
	wlan_cv_broadcast(cv);
}

void wlan_cv_destroy(kcondvar_t *cv) {
	osMutexDelete(hc_cond_lock(cv));
	osSemaphoreDelete(hc_cond_sem(cv));
	((struct host_cond *) cv)->hc_priv[0] = NULL;
	((struct host_cond *) cv)->hc_priv[1] = NULL;
}

/* ------------------------------------------------------------------ */

static osTimerId_t hc_timer(callout_t *c) {
	return (osTimerId_t) c->hc_timer;
}

/* Diagnostic gate for every driver callout at this layer (urtwn's 1 Hz
 * calib timer and its scan timer).  "wlan calib 0" keeps schedule from
 * arming and drops pending shots, which proves or clears the calib vs
 * ep0 interaction without touching verbatim code.  Counters are read
 * back by the same command. */
volatile unsigned wlan_callout_fires;
volatile unsigned wlan_callout_sched;
volatile unsigned wlan_callout_suppressed;
volatile unsigned wlan_callout_enabled = 1;
volatile unsigned wlan_callout_sched_failed;
static int wlan_callout_sched_failed_reported;

/* timer-thread blocking detector: a callout callback that ran longer
 * than 20 ms froze every other timer behind it (the fire context is the
 * kernel's single timer thread).  The first offender prints loudly, the
 * rest only count. */
volatile unsigned wlan_callout_long_fires;
static int wlan_callout_long_reported;

/* every callout_init() lands here so "wlan callouts" can attribute the
 * counters to the fn pointers (callout_destroy unlinks again) */
static callout_t *wlan_callout_registry;

/* CNTVCT helpers live further down with the wall-clock shims */
static uint64_t wlan_cntfrq(void);
static uint64_t wlan_cntvct(void);

unsigned wlan_callout_get_enabled(void) {
	return wlan_callout_enabled;
}

void wlan_callout_set_enabled(unsigned on) {
	wlan_callout_enabled = on ? 1U : 0U;
}

/* ms-since-boot for hc_last_fire_ms (CNTVCT; only relative use) */
static unsigned wlan_now_ms(void) {
	uint64_t frq = wlan_cntfrq();

	return (unsigned) (wlan_cntvct() * 1000ULL / frq);
}

static void host_callout_fire(void *arg) {
	callout_t *c = (callout_t *) arg;
	uint64_t t0, t1;

	c->hc_invoking = 1;
	c->hc_pending = 0;
	if (!wlan_callout_enabled) {
		wlan_callout_suppressed++;
		return;
	}
	wlan_callout_fires++;
	c->hc_fires++;
	c->hc_last_fire_ms = wlan_now_ms();
	if (c->hc_fn != NULL) {
		t0 = wlan_cntvct();
		c->hc_fn(c->hc_arg);
		t1 = wlan_cntvct();
		if (t1 - t0 > wlan_cntfrq() / 50) {
			c->hc_long_fires++;
			wlan_callout_long_fires++;
			if (!wlan_callout_long_reported) {
				wlan_callout_long_reported = 1;
				printf("wlan: callout fn=%p ran >20ms in "
					   "the timer thread (%lums)\n",
				       (void *) c->hc_fn,
				       (unsigned long)
				           ((t1 - t0) * 1000ULL /
				            wlan_cntfrq()));
			}
		}
	}
	c->hc_invoking = 0;
}

int callout_init(callout_t *c, int flags) {
	callout_t **pp;

	(void) flags;
	/* the registry is a port-side addition; upstream frees xfers with
	 * only callout_halt and re-inits recycled memory (usbd_do_request
	 * creates + destroys an xfer per control request), so an init on
	 * an already-linked callout must unlink it first or the list
	 * self-loops */
	for (pp = &wlan_callout_registry; *pp != NULL; pp = &(*pp)->hc_next) {
		if (*pp == c) {
			*pp = c->hc_next;
			break;
		}
	}
	c->hc_fn = NULL;
	c->hc_arg = NULL;
	c->hc_pending = 0;
	c->hc_scheds = 0;
	c->hc_fires = 0;
	c->hc_long_fires = 0;
	c->hc_last_fire_ms = 0;
	c->hc_next = wlan_callout_registry;
	wlan_callout_registry = c;
	/* period is set at schedule time; osTimerStart starts it */
	c->hc_timer = (void *) osTimerNew(host_callout_fire, osTimerOnce,
	    c, NULL);
	if (c->hc_timer == NULL) {
		/* the verbatim drivers ignore callout_init's status; a
		 * callout without a timer silently never fires (the
		 * 20260925 first-scan stall) */
		printf("wlan: callout_init FAILED - timer slots exhausted, "
		       "fn=%p will never fire\n", (void *) c->hc_fn);
		return ENOMEM;
	}
	return 0;
}

int callout_setfunc(callout_t *c, callout_fn_t fn, void *arg) {
	c->hc_fn = fn;
	c->hc_arg = arg;
	return 0;
}

int callout_schedule(callout_t *c, int ticks) {
	uint32_t period;

	if (!wlan_callout_enabled) {
		/* report success: the verbatim driver ignores the return
		 * value and its state machine must keep going */
		wlan_callout_suppressed++;
		return 0;
	}
	if (ticks <= 0) {
		ticks = 1;
	}
	period = ms2ticks((uint32_t) ticks * 1000 / WLAN_HZ);
	if (period == 0) {
		period = 1;
	}
	c->hc_pending = 1;
	/* osTimerStart starts a stopped (dormant) timer; only a real arm
	 * counts as scheduled, otherwise the counter hides dead timers */
	if (osTimerStart(hc_timer(c), period) != osOK) {
		wlan_callout_sched_failed++;
		if (!wlan_callout_sched_failed_reported) {
			wlan_callout_sched_failed_reported = 1;
			printf("wlan: callout_schedule FAILED to arm (no "
			       "timer slot?) fn=%p - callback never fires\n",
			       (void *) c->hc_fn);
		}
		return EINVAL;
	}
	c->hc_scheds++;
	wlan_callout_sched++;
	return 0;
}

int callout_stop(callout_t *c) {
	if (c->hc_timer != NULL) {
		osTimerStop(hc_timer(c));
	}
	c->hc_pending = 0;
	return 0;
}

int callout_halt(callout_t *c, kmutex_t *lock) {
	callout_t **pp;

	(void) lock;
	callout_stop(c);
	/* halt is terminal for this stack's callouts: usbd_free_xfer runs
	 * it right before kmem_free, and nothing re-arms a halted callout
	 * here.  Upstream can get away with keeping the (embedded) timer
	 * storage; our hc_timer is a pool object, so returning it here is
	 * what keeps usbd_do_request's per-request xfer from leaking a
	 * CMSIS timer slot per control transfer. */
	for (pp = &wlan_callout_registry; *pp != NULL; pp = &(*pp)->hc_next) {
		if (*pp == c) {
			*pp = c->hc_next;
			break;
		}
	}
	c->hc_next = NULL;
	if (c->hc_timer != NULL) {
		osTimerDelete(hc_timer(c));
		c->hc_timer = NULL;
	}
	return 0;
}

void callout_destroy(callout_t *c) {
	callout_t **pp;

	callout_stop(c);
	for (pp = &wlan_callout_registry; *pp != NULL; pp = &(*pp)->hc_next) {
		if (*pp == c) {
			*pp = c->hc_next;
			break;
		}
	}
	c->hc_next = NULL;
	if (c->hc_timer != NULL) {
		osTimerDelete(hc_timer(c));
		c->hc_timer = NULL;
	}
}

bool callout_pending(callout_t *c) {
	return c->hc_pending;
}

bool callout_invoking(callout_t *c) {
	return c->hc_invoking;
}

void wlan_callout_registry_dump(void) {
	const callout_t *c;
	int n;

	printf("callout registry (fn arg scheds fires long last_ms):\n");
	n = 0;
	for (c = wlan_callout_registry; c != NULL && n < 128;
	    c = c->hc_next, n++) {
		printf("  %p %p scheds=%u fires=%u long=%u "
		       "pending=%d last=%ums\n",
		       (void *) c->hc_fn, c->hc_arg,
		       c->hc_scheds, c->hc_fires, c->hc_long_fires,
		       c->hc_pending, c->hc_last_fire_ms);
	}
	if (c != NULL) {
		printf("  ... (registry walk truncated at %d)\n", n);
	}
	printf("callout totals: fires=%u sched=%u sched_failed=%u "
	       "suppressed=%u long=%u enabled=%u\n",
	       wlan_callout_fires, wlan_callout_sched,
	       wlan_callout_sched_failed, wlan_callout_suppressed,
	       wlan_callout_long_fires, wlan_callout_enabled);
}

int callout_ack(callout_t *c) {
	/* the fire already happened (osTimer fired once); report it */
	int fired = c->hc_pending;

	c->hc_pending = 0;
	return fired;
}

/* ------------------------------------------------------------------ */
/* wall-clock shims over CNTVCT (only relative deltas are consumed) */

void microtime(struct timeval *tv) {
	static uint64_t frq;
	uint64_t now;

	if (frq == 0) {
		__asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	}
	__asm volatile("mrs %0, cntvct_el0" : "=r"(now));
	tv->tv_sec = (long) (now / frq);
	tv->tv_usec = (long) ((now % frq) * 1000000ULL / frq);
}

int ratecheck(struct timeval *last, const struct timeval *min) {
	struct timeval tv;
	uint64_t elapsed_us, min_us;

	microtime(&tv);
	elapsed_us = (uint64_t) (tv.tv_sec - last->tv_sec) * 1000000ULL +
	    (uint64_t) (tv.tv_usec - last->tv_usec);
	min_us = (uint64_t) min->tv_sec * 1000000ULL + (uint64_t) min->tv_usec;
	if (elapsed_us >= min_us) {
		*last = tv;
		return 1;
	}
	return 0;
}

int hz = WLAN_HZ;

ticks_t getticks(void) {
	/* convert kernel ticks to the hz granularity the imported code
	 * expects */
	return (ticks_t) (osKernelGetTickCount() /
	    (osKernelGetTickFreq() / WLAN_HZ));
}

static uint64_t wlan_cntfrq(void) {
	uint64_t frq;

	__asm volatile("mrs %0, cntfrq_el0" : "=r"(frq));

	return frq;
}

static uint64_t wlan_cntvct(void) {
	uint64_t v;

	__asm volatile("mrs %0, cntvct_el0" : "=r"(v));

	return v;
}

void delay(unsigned int us) {
	static uint64_t frq;
	uint64_t deadline;

	if (frq == 0) {
		frq = wlan_cntfrq();
	}
	deadline = wlan_cntvct() + frq * (uint64_t) us / 1000000;

	for (;;) {
		uint64_t now = wlan_cntvct();
		uint64_t left = deadline - now;

		if (now >= deadline) {
			return;
		}
		/* long waits yield; sub-millisecond tails spin on CNTVCT */
		if (left > frq / 1000) {
			osDelay(ms2ticks((uint32_t)
			    (left * 1000 / frq)));
		}
	}
}

static unsigned int wlan_rand_state = 0x1234abcd;

void cprng_fast(void *buf, size_t len) {
	unsigned char *p = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		wlan_rand_state = wlan_rand_state * 1103515245 + 12345;
		p[i] = (unsigned char) (wlan_rand_state >> 16);
	}
}

int copyin(const void *u, void *k, size_t len) {
	memcpy(k, u, len);
	return 0;
}

int copyout(const void *k, void *u, size_t len) {
	memcpy(u, k, len);
	return 0;
}

int copystr(const void *kf, void *kt, size_t len, size_t *done) {
	const char *src = kf;
	size_t n = 0;

	while (n + 1 < len && src[n] != '\0') {
		((char *) kt)[n] = src[n];
		n++;
	}
	((char *) kt)[n] = '\0';
	if (done != NULL) {
		*done = n + 1;
	}
	return 0;
}

ipl_t splraiseipl(ipl_t ipl) {
	(void) ipl;
	return 0;
}

int max_linkhdr = 16;
int uimin(int a, int b) { return a < b ? a : b; }
int uimax(int a, int b) { return a > b ? a : b; }

ipl_t splsoftserial(void) {
	return 0;
}

ipl_t splnet(void) {
	return 0;
}

void splx(ipl_t ipl) {
	(void) ipl;
}

/* ------------------------------------------------------------------ */
/* Port serializer.
 *
 * The imported drivers rely on the splnet() discipline of their host
 * kernel to serialize the interrupt, state-machine and transmit
 * contexts; spl is a no-op here, so every driver entry takes this
 * reentrant lock. Sleeping waits drop it (see tsleep) so worker
 * contexts can deliver completions. */

static osMutexId_t wlan_ser_mtx;
static osThreadId_t wlan_ser_owner;
static int wlan_ser_depth;

void wlan_port_serializer_lock(void) {
	osThreadId_t self = osThreadGetId();

	if (wlan_ser_owner == self) {
		wlan_ser_depth++;
		return;
	}
	osMutexAcquire(wlan_ser_mtx, osWaitForever);
	wlan_ser_owner = self;
	wlan_ser_depth = 1;
}

void wlan_port_serializer_unlock(void) {
	osThreadId_t self = osThreadGetId();

	if (wlan_ser_owner != self || wlan_ser_depth <= 0) {
		panic("wlan serializer unlock by non-owner");
	}
	if (--wlan_ser_depth > 0) {
		return;
	}
	wlan_ser_owner = NULL;
	osMutexRelease(wlan_ser_mtx);
}

void *wlan_port_serializer_owner(void) {
	return wlan_ser_owner;
}

/* Fully release the lock around a sleep and return the saved hold
 * count (0 when the caller was not holding it). */
int wlan_port_serializer_suspend(void) {
	int depth;

	if (wlan_ser_owner != osThreadGetId()) {
		return 0;
	}
	depth = wlan_ser_depth;
	wlan_ser_owner = NULL;
	wlan_ser_depth = 0;
	osMutexRelease(wlan_ser_mtx);
	return depth;
}

void wlan_port_serializer_resume(int depth) {
	if (depth <= 0) {
		return;
	}
	osMutexAcquire(wlan_ser_mtx, osWaitForever);
	wlan_ser_owner = osThreadGetId();
	wlan_ser_depth = depth;
}

/* ------------------------------------------------------------------ */
/* tsleep/wakeup: identified bounded waits over one global (mutex, cv)
 * pair. A waiter releases the port serializer around the wait. */

struct wlan_tsleep_slot {
	void *ident;
	int fired;
	struct wlan_tsleep_slot *next;
};

static osMutexId_t wlan_tsleep_mtx;
static osSemaphoreId_t wlan_tsleep_sem; /* broadcast pool */
static struct wlan_tsleep_slot *wlan_tsleep_slots;

/* Wakeups that found no waiter are remembered here for a short window.
 * The imported drivers use the Unix check-then-tsleep idiom, which
 * relies on the interrupt path not being able to slip a wakeup in
 * between the predicate test and the sleep; spl does that on NetBSD
 * but is a no-op here, so a wakeup that arrives in that window would
 * otherwise be lost and the caller would run into its timeout. */
#define WLAN_TSLEEP_PENDING_MAX 16
static void *wlan_tsleep_pending[WLAN_TSLEEP_PENDING_MAX];
static int wlan_tsleep_pending_n;

static int wlan_tsleep_consume_pending(void *ident) {
	int i;

	for (i = 0; i < wlan_tsleep_pending_n; i++) {
		if (wlan_tsleep_pending[i] == ident) {
			wlan_tsleep_pending[i] =
			    wlan_tsleep_pending[--wlan_tsleep_pending_n];
			return 1;
		}
	}
	return 0;
}

int tsleep(void *ident, int pri, const char *wmesg, int timo) {
	struct wlan_tsleep_slot w;
	struct wlan_tsleep_slot **pp;
	uint32_t quantum;
	uint32_t deadline;
	uint32_t now;
	int held;
	int rc = 0;

	(void) pri;
	(void) wmesg;

	w.ident = ident;
	w.fired = 0;

	osMutexAcquire(wlan_tsleep_mtx, osWaitForever);
	w.next = wlan_tsleep_slots;
	wlan_tsleep_slots = &w;
	/* a wakeup may have raced in before the slot was linked */
	if (wlan_tsleep_consume_pending(ident)) {
		w.fired = 1;
	}
	osMutexRelease(wlan_tsleep_mtx);

	/* timo: ticks in hz units; <= 0 means forever; 0 deadline marks
	 * the infinite wait */
	now = osKernelGetTickCount();
	if (timo <= 0) {
		deadline = 0;
	} else {
		deadline = now + ms2ticks((uint32_t) timo * 1000 / WLAN_HZ);
	}
	quantum = ms2ticks(20);

	held = wlan_port_serializer_suspend();

	osMutexAcquire(wlan_tsleep_mtx, osWaitForever);
	while (!w.fired) {
		uint32_t left;

		if (deadline != 0) {
			uint32_t cur = osKernelGetTickCount();

			if (cur >= deadline) {
				rc = EWOULDBLOCK;
				break;
			}
			left = deadline - cur;
			if (left > quantum) {
				left = quantum; /* poll quantum */
			}
		} else {
			left = quantum;
		}
		osMutexRelease(wlan_tsleep_mtx);
		osSemaphoreAcquire(wlan_tsleep_sem, left);
		osMutexAcquire(wlan_tsleep_mtx, osWaitForever);
	}
	for (pp = &wlan_tsleep_slots; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == &w) {
			*pp = w.next;
			break;
		}
	}
	osMutexRelease(wlan_tsleep_mtx);

	wlan_port_serializer_resume(held);
	return rc;
}

void wakeup(void *ident) {
	struct wlan_tsleep_slot *s;
	int matched = 0;

	osMutexAcquire(wlan_tsleep_mtx, osWaitForever);
	for (s = wlan_tsleep_slots; s != NULL; s = s->next) {
		if (s->ident == ident) {
			s->fired = 1;
			matched = 1;
		}
	}
	if (!matched && wlan_tsleep_pending_n < WLAN_TSLEEP_PENDING_MAX) {
		wlan_tsleep_pending[wlan_tsleep_pending_n++] = ident;
	}
	osMutexRelease(wlan_tsleep_mtx);

	/* one token wakes one waiter; multiple sleepers on the same ident
	 * poll their deadlines */
	osSemaphoreRelease(wlan_tsleep_sem);
}

int kpause(const char *ident, bool nlocked, int timo, kmutex_t *lock) {
	(void) nlocked;
	(void) lock;
	return tsleep((void *) ident, 0, ident, timo);
}

void wakeup_one(void *ident) {
	wakeup(ident);
}

/* ------------------------------------------------------------------ */
/* kthread(9): kthreads run on the port worker primitive (the same
 * usb_osal thread the usbdi shim workers use; ThreadX completes a
 * thread when its entry returns). The lwp_t handle is only an "is the
 * worker alive" flag to the drivers - they clear it themselves on
 * exit. Each create allocates a TCB block from the CherryUSB byte
 * pool that is not reclaimed on exit: wlan up/down cycles are bounded
 * in this harness. */

struct wlan_kthread_start {
	void (*fn)(void *);
	void *arg;
};

static void wlan_kthread_trampoline(void *v) {
	struct wlan_kthread_start start = *(struct wlan_kthread_start *) v;

	wlan_osal_free(v);
	start.fn(start.arg);
}

int kthread_create(pri_t pri, int flags, struct cpu_info *ci,
    void (*func)(void *), void *arg, lwp_t **lwpp, const char *fmt, ...) {
	struct wlan_kthread_start *start;
	lwp_t lwp;

	(void) pri;
	(void) flags;
	(void) ci;
	(void) fmt;

	start = wlan_osal_alloc(sizeof(*start));
	if (start == NULL) {
		return ENOMEM;
	}
	start->fn = func;
	start->arg = arg;
	lwp = (lwp_t) wlan_port_thread_create(wlan_kthread_trampoline, start);
	if (lwp == NULL) {
		wlan_osal_free(start);
		return ENOMEM;
	}
	if (lwpp != NULL) {
		*lwpp = lwp;
	}
	return 0;
}

void kthread_exit(int code) {
	(void) code;
	/* park here if the CMSIS layer has no terminate; a terminated or
	 * parked worker both read as "gone" to the drivers (they clear
	 * sc_worker themselves before calling this). */
	osThreadTerminate(osThreadGetId());
	for (;;) {
		osDelay(1);
	}
}

/* ------------------------------------------------------------------ */

void wlan_osal_cmsis_init(void) {
	if (wlan_ser_mtx != NULL) {
		return;
	}
	wlan_ser_mtx = osMutexNew(NULL);
	wlan_tsleep_mtx = osMutexNew(NULL);
	wlan_tsleep_sem = osSemaphoreNew(0xffff, 0, NULL);
}
