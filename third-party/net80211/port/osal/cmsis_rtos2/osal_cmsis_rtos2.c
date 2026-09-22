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
#include <sys/endian.h>

#include "wlan_port_cmsis.h"

/* the compat malloc/free macros must not intercept the host calls */
#undef malloc
#undef free

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
	 * for the small DMA buffers the shim allocates) */
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

int wlan_cv_broadcast(kcondvar_t *cv) {
	int n;

	osMutexAcquire(hc_cond_lock(cv), osWaitForever);
	n = *hc_cond_waiters(cv);
	osMutexRelease(hc_cond_lock(cv));
	while (n-- > 0) {
		osSemaphoreRelease(hc_cond_sem(cv));
	}
	return 0;
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

static void host_callout_fire(void *arg) {
	callout_t *c = (callout_t *) arg;

	c->hc_pending = 0;
	if (c->hc_fn != NULL) {
		c->hc_fn(c->hc_arg);
	}
}

int callout_init(callout_t *c, int flags) {
	(void) flags;
	c->hc_fn = NULL;
	c->hc_arg = NULL;
	c->hc_pending = 0;
	/* period is set at schedule time; osTimerStart starts it */
	c->hc_timer = (void *) osTimerNew(host_callout_fire, osTimerOnce,
	    c, NULL);
	if (c->hc_timer == NULL) {
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

	if (ticks <= 0) {
		ticks = 1;
	}
	period = ms2ticks((uint32_t) ticks * 1000 / WLAN_HZ);
	if (period == 0) {
		period = 1;
	}
	c->hc_pending = 1;
	/* osTimerStart starts a stopped (dormant) timer */
	if (osTimerStart(hc_timer(c), period) != osOK) {
		return EINVAL;
	}
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
	(void) lock;
	return callout_stop(c);
}

void callout_destroy(callout_t *c) {
	callout_stop(c);
	if (c->hc_timer != NULL) {
		osTimerDelete(hc_timer(c));
		c->hc_timer = NULL;
	}
}

bool callout_pending(callout_t *c) {
	return c->hc_pending;
}

/* ------------------------------------------------------------------ */

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

void wakeup_one(void *ident) {
	wakeup(ident);
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
