/*
 * @file   cmsis_os2_impl.c
 * @brief  CMSIS-RTOS2 (include/cmsis_os2.h) implemented on Eclipse ThreadX
 *         SMP (common_smp + the cortex_a55_smp port).
 *
 * The ThreadX twin of port/adapters/cmsis_rtos2/cmsis_os2_impl.c - the
 * kernel-replacement seam, second instance. Everything above it (drivers,
 * app/, the cherrysh adapter) calls the `os*` API and never ThreadX. This
 * file is what the "内核替换缝" claim (DESIGN §4.4, K3/K4) cashes in on: the
 * same app/ + drivers/ objects link against either kernel.
 *
 * Scope (D39): the comparison image's CONSUMED surface, not the full 48-API
 * port. Coverage, stated honestly:
 *
 *   IMPLEMENTED - kernel control, threads (+ self/other terminate), thread
 *   flags (per-thread TX_EVENT_FLAGS_GROUP), delays, event flags, counting
 *   semaphores, non-recursive mutexes, message queues, timers, memory
 *   pools. Timers and timer callbacks run in the kernel's timer thread,
 *   exactly like the FreeRTOS image's timer task.
 *
 *   DEFERRED CREATION - ThreadX objects may only be created from
 *   tx_application_define (or later). board_main() creates its threads
 *   between osKernelInitialize() and osKernelStart(), which is fine on
 *   FreeRTOS and illegal here. Every creation in that window therefore
 *   allocates its slot immediately (the handle, being the slot address, is
 *   stable from that moment) and records the real constructor; the
 *   constructors replay inside tx_cmsis_application_define(), which
 *   tx_application_define calls.
 *
 *   DELIBERATE DIFFERENCES, each a ThreadX limitation stated at the site:
 *   - semaphores are unbounded: osSemaphoreNew's max_count is recorded but
 *     not enforced (ThreadX has no ceiling);
 *   - mutexes are not recursive: an osMutexRecursive attr is rejected, the
 *     same honesty policy as rejecting cb_mem;
 *   - message queues copy in ULONG words: msg_size is rounded up and
 *     callers' buffers must tolerate up to 3 bytes of padding, which every
 *     in-tree caller does;
 *   - osKernelLock/Unlock/RestoreLock map onto the port's global protection
 *     (_tx_thread_smp_protect, the TX_DISABLE primitive): cross-core mutual
 *     exclusion with local IRQs held off. That is STRONGER than the
 *     FreeRTOS twin's vTaskSuspendAll, which stops only local scheduling
 *     and keeps IRQs on - on four cores a local-only lock protects nothing,
 *     so the global primitive is the honest analog. The nesting-count
 *     contract (return depth / restore to depth) matches the twin exactly.
 *
 * Static allocation: ThreadX wants caller-provided control blocks and
 * stacks for everything, so every object type has a static slot pool and a
 * caller-supplied cb_mem is rejected (osErrorParameter / NULL) rather than
 * accepted and ignored - identical policy to the FreeRTOS twin.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"
#include "tx_thread.h"
#include "tx_event_flags.h"
#include "tx_semaphore.h"
#include "tx_mutex.h"
#include "tx_queue.h"
#include "tx_timer.h"

#include "cmsis_os2.h"
#include "cmsis_os2_ext.h"

/* asm hand-off stubs (port/adapters/threadx/tx_vectors.S) */
extern void tx_kernel_enter_spsel0(void);

/* board SMP release (port/aarch64/smp.c, kernel-agnostic) */
extern void board_smp_start_secondaries(void);

/* The port assembly's global protection (tx_thread_smp_protect.S): what
 * TX_DISABLE expands to on this port. See the kernel-lock section. The
 * prototypes come from tx_api.h. */

/* --- priority and timeout translation ------------------------------------ */

/* CMSIS: 1..56, higher = more urgent. ThreadX: 0..TX_MAX_PRIORITIES-1,
 * LOWER = more urgent. CMSIS named bands are 8 apart; each band maps to a
 * run of 4 ThreadX levels, highest band first. osPriorityNormal (24, band
 * 3) lands on level 16 - the middle of the range, same shape as the
 * FreeRTOS twin's default. */
static UINT to_tx_priority(osPriority_t priority)
{
	uint32_t band;

	if ((int32_t)priority <= (int32_t)osPriorityIdle) {
		band = 0U;
	} else {
		band = (uint32_t)priority / 8U;
		if (band > 7U) {
			band = 7U;
		}
	}
	return (UINT)((TX_MAX_PRIORITIES - 1U) - band * 4U);
}

/* ThreadX timeouts are raw ticks, TX_WAIT_FOREVER is 0xFFFFFFFF - the same
 * value osWaitForever uses, so the mapping is the identity. */
static ULONG to_ticks(uint32_t timeout)
{
	return (timeout == osWaitForever) ? (ULONG)TX_WAIT_FOREVER
					  : (ULONG)timeout;
}

static osStatus_t wait_result(UINT tx_status, uint32_t timeout)
{
	switch (tx_status) {
	case TX_SUCCESS:
		return osOK;
	case TX_NO_INSTANCE:
	case TX_NO_EVENTS:
	case TX_NOT_AVAILABLE:
	case TX_SEMAPHORE_ERROR:
	/* "would have to wait" cases: report by intent, same policy as the
	 * FreeRTOS twin (0 timeout = osErrorResource, else osErrorTimeout). */
	default:
		break;
	}
	return (timeout == 0U) ? osErrorResource : osErrorTimeout;
}

/* --- deferred creation ----------------------------------------------------- */

typedef enum {
	DEF_THREAD,
	DEF_EVENT_FLAGS,
	DEF_SEMAPHORE,
	DEF_MUTEX,
	DEF_QUEUE,
	DEF_TIMER,
} def_kind_t;

typedef struct {
	def_kind_t kind;
	void      *slot;
	/* Arguments, per kind - stored as words to keep one record shape.
	 * DEF_THREAD:        func, arg, priority, stack_words
	 * DEF_EVENT_FLAGS:   -
	 * DEF_SEMAPHORE:     initial_count
	 * DEF_MUTEX:         (none)
	 * DEF_QUEUE:         msg_count, msg_words
	 * DEF_TIMER:         periodic(0/1)
	 */
	uintptr_t  a0, a1, a2, a3;
} def_record_t;

#define MAX_DEFERRED	32

static def_record_t deferrals[MAX_DEFERRED];
static uint32_t deferral_count;
static uint8_t  kernel_objects_live;	/* set once replay has run */

static int defer(def_kind_t kind, void *slot,
		 uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
	if (kernel_objects_live) {
		return 1;	/* kernel is up: create immediately */
	}
	if (deferral_count >= MAX_DEFERRED) {
		return 0;
	}
	deferrals[deferral_count].kind = kind;
	deferrals[deferral_count].slot = slot;
	deferrals[deferral_count].a0 = a0;
	deferrals[deferral_count].a1 = a1;
	deferrals[deferral_count].a2 = a2;
	deferrals[deferral_count].a3 = a3;
	deferral_count++;
	return 1;
}

/* --- threads --------------------------------------------------------------- */

#define MAX_THREADS		32
#define THREAD_STACK_DEFAULT	2048u		/* 8 KiB, the long-standing default */
#define THREAD_STACK_MIN	256u		/* 1 KiB floor */
#define THREAD_STACK_MAX_BYTES	(128u * 1024u)

typedef enum {
	SLOT_FREE = 0,
	SLOT_PENDING,		/* allocated, created at replay time */
	SLOT_LIVE,
	SLOT_ZOMBIE,		/* self-terminated/finished; reap on next New */
} slot_state_t;

typedef struct {
	TX_THREAD             thread;
	TX_EVENT_FLAGS_GROUP  tflags;	/* this thread's CMSIS thread flags */
	ULONG                *stack;	/* heap (port/adapters/threadx/heap.c);
					 * sized per attr->stack_size, freed on
					 * reap/delete - a fixed 8 KiB per slot
					 * x 32 slots would burn 256 KiB of
					 * .bss on threads that ask for less */
	uint32_t              stack_words;
	osThreadFunc_t        func;
	void                 *arg;
	slot_state_t          state;
} thread_slot_t;

static thread_slot_t thread_slots[MAX_THREADS];

/* Allocation surface shared with the middleware adapters (sdmmc OSA,
 * CherryUSB osal). Prototypes here rather than a header: the only two
 * consumers in this file are the stack allocator and the reaper. */
extern void *pvPortMalloc(size_t length);
extern void vPortFree(void *ptr);

static ULONG *thread_stack_alloc(uint32_t stack_size, uint32_t *words_out)
{
	uint32_t bytes;
	ULONG *stack;

	bytes = (stack_size != 0U) ? stack_size
				   : THREAD_STACK_DEFAULT * sizeof(ULONG);
	if (bytes > THREAD_STACK_MAX_BYTES) {
		return NULL;
	}
	bytes = (bytes + 15U) & ~15U;	/* AArch64 stack alignment */
	stack = (ULONG *)pvPortMalloc(bytes);
	if (stack != NULL) {
		*words_out = bytes / (uint32_t)sizeof(ULONG);
	}
	return stack;
}

static thread_slot_t *thread_slot_alloc(void)
{
	uint32_t i;

	for (i = 0U; i < MAX_THREADS; i++) {
		if (thread_slots[i].state == SLOT_FREE) {
			return &thread_slots[i];
		}
	}
	return NULL;
}

/* Reap zombies: threads that terminated THEMSELVES (osThreadTerminate(self)
 * or falling off the entry function). ThreadX forbids deleting a thread
 * from its own context, so the slot was parked as ZOMBIE; a LATER
 * osThreadNew call - necessarily running in some other thread - finishes
 * the job. The thread must be in TX_COMPLETED / TX_TERMINATED_STATE and
 * must not be the caller. */
static void thread_zombies_reap(void)
{
	uint32_t i;

	for (i = 0U; i < MAX_THREADS; i++) {
		thread_slot_t *slot = &thread_slots[i];

		if (slot->state != SLOT_ZOMBIE ||
		    (TX_THREAD *)&slot->thread ==
			    (TX_THREAD *)tx_thread_identify()) {
			continue;
		}
		if (slot->thread.tx_thread_state != TX_COMPLETED &&
		    slot->thread.tx_thread_state != TX_TERMINATED) {
			continue;
		}
		if (tx_thread_delete(&slot->thread) == TX_SUCCESS) {
			(void)tx_event_flags_delete(&slot->tflags);
			vPortFree(slot->stack);
			slot->stack = NULL;
			slot->state = SLOT_FREE;
		}
	}
}

static void thread_trampoline(ULONG argument)
{
	thread_slot_t *slot = (thread_slot_t *)(uintptr_t)argument;

	slot->func(slot->arg);

	/* Natural completion: ThreadX puts the thread in TX_COMPLETED and
	 * never returns here. Park the slot for the zombie reaper. */
	slot->state = SLOT_ZOMBIE;
}

static void thread_slot_init_flags(thread_slot_t *slot)
{
	/* One event-flags group per thread IS the CMSIS thread-flags
	 * implementation (see osThreadFlagsSet). Created here in kernel
	 * context, one line after the thread it belongs to. */
	(void)tx_event_flags_create(&slot->tflags, (CHAR *)"cmsis-tflags");
	slot->thread.tx_thread_cmsis_slot = slot;
}

static void thread_create_into_slot(thread_slot_t *slot, osThreadFunc_t func,
				    void *argument, UINT priority,
				    uint32_t affinity_mask)
{
	slot->func = func;
	slot->arg = argument;

	if (tx_thread_create(&slot->thread, (CHAR *)"cmsis-thr",
			     thread_trampoline, (ULONG)(uintptr_t)slot,
			     (VOID *)slot->stack,
			     (ULONG)slot->stack_words * sizeof(ULONG),
			     priority, priority,
			     TX_NO_TIME_SLICE, TX_AUTO_START) != TX_SUCCESS) {
		vPortFree(slot->stack);
		slot->stack = NULL;
		slot->state = SLOT_FREE;
		return;
	}
	slot->state = SLOT_LIVE;
	thread_slot_init_flags(slot);

	/* CMSIS affinity_mask (bit i = core i allowed) maps onto ThreadX's
	 * EXCLUSION bitmap (bit i = core i forbidden) - the complement. A
	 * zero or full mask means "no constraint". smp_test's per-core
	 * pinning rides on this; dropping it silently would break the
	 * comparison workload's core-binding contract. */
	if (affinity_mask != 0U &&
	    (affinity_mask & (uint32_t)TX_THREAD_SMP_CORE_MASK) !=
		    (uint32_t)TX_THREAD_SMP_CORE_MASK) {
		(void)tx_thread_smp_core_exclude(&slot->thread,
						 (ULONG)~affinity_mask &
						 (ULONG)TX_THREAD_SMP_CORE_MASK);
	}
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
			 const osThreadAttr_t *attr)
{
	thread_slot_t *slot;
	UINT priority = to_tx_priority(osPriorityNormal);
	uint32_t affinity_mask = 0U;
	uint32_t stack_size = 0U;

	if (func == NULL) {
		return NULL;
	}
	thread_zombies_reap();

	slot = thread_slot_alloc();
	if (slot == NULL) {
		return NULL;
	}
	if (attr != NULL) {
		if (attr->cb_mem != NULL) {
			/* ThreadX owns the TCB inside the slot; accepting the
			 * caller's block and ignoring it would corrupt their
			 * memory later. Same policy as the FreeRTOS twin. */
			return NULL;
		}
		if (attr->priority != osPriorityNone) {
			priority = to_tx_priority(attr->priority);
		}
		affinity_mask = attr->affinity_mask;
		stack_size = attr->stack_size;
	}
	slot->stack = thread_stack_alloc(stack_size, &slot->stack_words);
	if (slot->stack == NULL) {
		slot->state = SLOT_FREE;
		return NULL;
	}

	if (kernel_objects_live) {
		/* Kernel running: create now, from a thread context. */
		thread_create_into_slot(slot, func, argument, priority,
					affinity_mask);
		if (slot->state != SLOT_LIVE) {
			return NULL;
		}
		return (osThreadId_t)&slot->thread;
	}

	/* Pre-kernel: the slot is allocated now (the handle - the TCB
	 * address - is stable from this moment), the constructor replays in
	 * tx_cmsis_application_define, the only sanctioned creation window.
	 * The stack is also allocated here - the heap works before the
	 * kernel starts - so the replay never allocates. */
	slot->state = SLOT_PENDING;
	if (!defer(DEF_THREAD, slot,
		   (uintptr_t)func, (uintptr_t)argument,
		   (uintptr_t)priority, (uintptr_t)affinity_mask)) {
		vPortFree(slot->stack);
		slot->stack = NULL;
		slot->state = SLOT_FREE;
		return NULL;
	}
	return (osThreadId_t)&slot->thread;
}

osThreadId_t osThreadGetId(void)
{
	return (osThreadId_t)tx_thread_identify();
}

osStatus_t osThreadYield(void)
{
	tx_thread_relinquish();
	return osOK;
}

osStatus_t osThreadTerminate(osThreadId_t thread_id)
{
	TX_THREAD *thread = (TX_THREAD *)thread_id;
	thread_slot_t *slot;

	if (thread_id == NULL) {
		return osErrorParameter;
	}
	slot = (thread_slot_t *)thread->tx_thread_cmsis_slot;
	if (slot == NULL || slot->state != SLOT_LIVE) {
		/* Kernel-created threads (idle, timer) and already-dead slots
		 * have nothing this API may touch. */
		return osErrorParameter;
	}

	if (thread == (TX_THREAD *)tx_thread_identify()) {
		/* Self-terminate: ThreadX forbids deleting a thread from its
		 * own context. Terminate takes effect at the next schedule
		 * point; the slot goes to the zombie reaper. */
		slot->state = SLOT_ZOMBIE;
		(void)tx_thread_terminate(thread);
		return osOK;
	}

	(void)tx_thread_terminate(thread);
	if (tx_thread_delete(thread) != TX_SUCCESS) {
		return osErrorParameter;
	}
	(void)tx_event_flags_delete(&slot->tflags);
	vPortFree(slot->stack);
	slot->stack = NULL;
	slot->state = SLOT_FREE;
	return osOK;
}

/* --- thread flags ---------------------------------------------------------- */
/* One TX_EVENT_FLAGS_GROUP per thread, backlinked from the TCB extension.
 * tx_event_flags_set is documented callable from ISRs, so the FromISR
 * extension needs no special path - the kernel's SMP preemption machinery
 * (preempt IPI + IRQ-exit reschedule) does what FreeRTOS needs an explicit
 * portYIELD_FROM_ISR for. */

static TX_EVENT_FLAGS_GROUP *thread_flags_of(osThreadId_t thread_id)
{
	TX_THREAD *thread = (TX_THREAD *)thread_id;

	if (thread_id == NULL) {
		return NULL;
	}
	return (TX_EVENT_FLAGS_GROUP *)thread->tx_thread_cmsis_slot != NULL
	       ? &((thread_slot_t *)thread->tx_thread_cmsis_slot)->tflags
	       : NULL;
}

static uint32_t flags_read_clear(TX_EVENT_FLAGS_GROUP *group, uint32_t flags,
				 UINT clear_option)
{
	ULONG actual = 0U;

	(void)tx_event_flags_get(group, (ULONG)flags, clear_option,
				 &actual, TX_NO_WAIT);
	return (uint32_t)actual;
}

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
	TX_EVENT_FLAGS_GROUP *group = thread_flags_of(thread_id);
	uint32_t previous;

	if (group == NULL || flags == 0U) {
		return (uint32_t)osError;
	}
	previous = flags_read_clear(group, 0xFFFFFFFFU, TX_OR);
	if (tx_event_flags_set(group, (ULONG)flags, TX_OR) != TX_SUCCESS) {
		return (uint32_t)osError;
	}
	/* CMSIS wants the flags value AFTER the set. */
	return previous | flags;
}

uint32_t osThreadFlagsSetFromISR(osThreadId_t thread_id, uint32_t flags)
{
	/* Same primitive (see cmsis_os2_ext.h for why this extension
	 * exists): tx_event_flags_set is ISR-callable, and a woken
	 * higher-priority thread preempts through the IRQ-exit reschedule
	 * without further help. */
	return osThreadFlagsSet(thread_id, flags);
}

uint32_t osThreadFlagsClear(uint32_t flags)
{
	TX_THREAD *self = (TX_THREAD *)tx_thread_identify();
	TX_EVENT_FLAGS_GROUP *group = thread_flags_of((osThreadId_t)self);

	if (group == NULL) {
		return (uint32_t)osError;
	}
	/* CMSIS wants the value as it was BEFORE the clear. */
	return flags_read_clear(group, flags,
				(flags != 0U) ? TX_OR_CLEAR : TX_OR);
}

uint32_t osThreadFlagsGet(void)
{
	TX_THREAD *self = (TX_THREAD *)tx_thread_identify();
	TX_EVENT_FLAGS_GROUP *group = thread_flags_of((osThreadId_t)self);

	if (group == NULL) {
		return (uint32_t)osError;
	}
	return flags_read_clear(group, 0xFFFFFFFFU, TX_OR);
}

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
	TX_THREAD *self = (TX_THREAD *)tx_thread_identify();
	TX_EVENT_FLAGS_GROUP *group = thread_flags_of((osThreadId_t)self);
	UINT clear_option;
	ULONG actual = 0U;
	UINT status;

	if (group == NULL || flags == 0U) {
		return (uint32_t)osError;
	}
	if ((options & osFlagsNoClear) != 0U) {
		clear_option = ((options & osFlagsWaitAll) != 0U) ? TX_AND
								  : TX_OR;
	} else {
		clear_option = ((options & osFlagsWaitAll) != 0U)
			       ? TX_AND_CLEAR : TX_OR_CLEAR;
	}
	status = tx_event_flags_get(group, (ULONG)flags, clear_option,
				    &actual, to_ticks(timeout));
	if (status != TX_SUCCESS) {
		return (timeout == 0U) ? (uint32_t)osErrorResource
				       : (uint32_t)osErrorTimeout;
	}
	return (uint32_t)actual;
}

/* --- delays ---------------------------------------------------------------- */

osStatus_t osDelay(uint32_t ticks)
{
	if (ticks != 0U) {
		(void)tx_thread_sleep((ULONG)ticks);
	}
	return osOK;
}

/* --- kernel ---------------------------------------------------------------- */

static osKernelState_t kernel_state = osKernelInactive;

osStatus_t osKernelInitialize(void)
{
	kernel_state = osKernelReady;
	return osOK;
}

osStatus_t osKernelStart(void)
{
	if (kernel_state != osKernelReady) {
		return osError;
	}
	/* Release the secondaries BEFORE the kernel enters, mirroring the
	 * FreeRTOS image's ordering (StartSecondaryCpuUp before the tick is
	 * armed): each released core runs its own GIC bring-up, reports in,
	 * and parks inside _tx_thread_smp_initialize_wait until kernel
	 * initialization releases it into the schedule loop. */
	board_smp_start_secondaries();

	kernel_state = osKernelRunning;
	tx_kernel_enter_spsel0();		/* never returns */

	kernel_state = osKernelError;
	return osError;
}

osKernelState_t osKernelGetState(void)
{
	return kernel_state;
}

osStatus_t osKernelGetInfo(osVersion_t *version, char *id_buf, uint32_t id_size)
{
	static const char kernel_id[] = "ThreadX SMP";

	if (version != NULL) {
		version->api = 20010003U;	/* CMSIS-RTOS2 API 2.1.3 */
		version->kernel = 0x06050102U;	/* ThreadX 6.5.1.202602a */
	}
	if (id_buf != NULL && id_size > 0U) {
		uint32_t n = (uint32_t)sizeof(kernel_id);

		if (n > id_size) {
			n = id_size;
		}
		(void)memcpy(id_buf, kernel_id, n);
		id_buf[n - 1U] = '\0';
	}
	return osOK;
}

uint32_t osKernelGetTickCount(void)
{
	/* Kernel tick, in milliseconds at the tick glue's 1000 Hz. */
	return (uint32_t)tx_time_get();
}

uint32_t osKernelGetTickFreq(void)
{
	/* Must match TX_TICK_RATE_HZ in tx_glue.c; both mirror the FreeRTOS
	 * image's configTICK_RATE_HZ so the comparison runs the same tick. */
	return 1000U;
}

uint32_t osKernelGetSysTimerCount(void)
{
	return osKernelGetTickCount();
}

uint32_t osKernelGetSysTimerFreq(void)
{
	return osKernelGetTickFreq();
}

/* --- kernel lock ------------------------------------------------------------ */
/* The OSA-style critical sections (the sdmmc shadow's OSA_ENTER_CRITICAL,
 * port/adapters/sdmmc/shadow/fsl_os_abstraction.h) ride on these. ThreadX
 * has no scheduler-suspend primitive; the port's global protection is the
 * honest SMP analog (see the file-header note). protect() returns the
 * caller's DAIF, which must come back in reverse order - so the save
 * values live on a per-depth stack. Depth 8 is generous: these sections
 * guard a word of state and never span a blocking call, but running out is
 * reported rather than silently corrupting the save stack. */
#define MAX_KERNEL_LOCK_DEPTH	8

static uint32_t kernel_lock_count;
static UINT kernel_lock_daif[MAX_KERNEL_LOCK_DEPTH];

int32_t osKernelLock(void)
{
	if (kernel_state != osKernelRunning) {
		return (int32_t)osError;
	}
	if (kernel_lock_count >= MAX_KERNEL_LOCK_DEPTH) {
		return (int32_t)osError;
	}
	kernel_lock_daif[kernel_lock_count] = _tx_thread_smp_protect();
	kernel_lock_count++;
	return (int32_t)kernel_lock_count;
}

int32_t osKernelUnlock(void)
{
	if (kernel_state != osKernelRunning) {
		return (int32_t)osError;
	}
	if (kernel_lock_count > 0U) {
		kernel_lock_count--;
		_tx_thread_smp_unprotect(kernel_lock_daif[kernel_lock_count]);
	}
	return (int32_t)kernel_lock_count;
}

int32_t osKernelRestoreLock(int32_t lock)
{
	if (kernel_state != osKernelRunning) {
		return (int32_t)osError;
	}
	if (lock < 0) {
		return (int32_t)osErrorParameter;
	}
	while (kernel_lock_count > (uint32_t)lock) {
		(void)osKernelUnlock();
	}
	while (kernel_lock_count < (uint32_t)lock) {
		if (kernel_lock_count >= MAX_KERNEL_LOCK_DEPTH) {
			return (int32_t)osError;
		}
		kernel_lock_daif[kernel_lock_count] = _tx_thread_smp_protect();
		kernel_lock_count++;
	}
	return (int32_t)kernel_lock_count;
}

/* --- event flags ----------------------------------------------------------- */

#define MAX_EFLAGS	16

typedef struct {
	TX_EVENT_FLAGS_GROUP group;
	slot_state_t         state;
} eflags_slot_t;

static eflags_slot_t eflags_slots[MAX_EFLAGS];

osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr)
{
	eflags_slot_t *slot;
	uint32_t i;

	(void)attr;
	if (kernel_objects_live) {
		for (i = 0U; i < MAX_EFLAGS; i++) {
			if (eflags_slots[i].state == SLOT_FREE) {
				slot = &eflags_slots[i];
				break;
			}
		}
		if (i == MAX_EFLAGS) {
			return NULL;
		}
		if (tx_event_flags_create(&slot->group,
					  (CHAR *)"cmsis-ef") != TX_SUCCESS) {
			return NULL;
		}
		slot->state = SLOT_LIVE;
		return (osEventFlagsId_t)slot;
	}

	/* Pre-kernel: allocate the slot now, defer the constructor. */
	for (i = 0U; i < MAX_EFLAGS; i++) {
		if (eflags_slots[i].state == SLOT_FREE) {
			slot = &eflags_slots[i];
			slot->state = SLOT_PENDING;
			if (defer(DEF_EVENT_FLAGS, slot, 0, 0, 0, 0)) {
				return (osEventFlagsId_t)slot;
			}
			slot->state = SLOT_FREE;
			return NULL;
		}
	}
	return NULL;
}

uint32_t osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags)
{
	eflags_slot_t *slot = (eflags_slot_t *)ef_id;
	uint32_t previous;

	if (ef_id == NULL || flags == 0U ||
	    slot->state != SLOT_LIVE) {
		return (uint32_t)osError;
	}
	/* ISR-callable (tx_event_flags_set), like the CMSIS contract wants. */
	previous = flags_read_clear(&slot->group, 0xFFFFFFFFU, TX_OR);
	(void)tx_event_flags_set(&slot->group, (ULONG)flags, TX_OR);
	return previous | flags;
}

uint32_t osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags)
{
	eflags_slot_t *slot = (eflags_slot_t *)ef_id;

	if (ef_id == NULL || slot->state != SLOT_LIVE) {
		return (uint32_t)osError;
	}
	return flags_read_clear(&slot->group, flags,
				(flags != 0U) ? TX_OR_CLEAR : TX_OR);
}

uint32_t osEventFlagsGet(osEventFlagsId_t ef_id)
{
	eflags_slot_t *slot = (eflags_slot_t *)ef_id;

	if (ef_id == NULL || slot->state != SLOT_LIVE) {
		return (uint32_t)osError;
	}
	return flags_read_clear(&slot->group, 0xFFFFFFFFU, TX_OR);
}

uint32_t osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags,
			  uint32_t options, uint32_t timeout)
{
	eflags_slot_t *slot = (eflags_slot_t *)ef_id;
	UINT clear_option;
	ULONG actual = 0U;

	if (ef_id == NULL || flags == 0U || slot->state != SLOT_LIVE) {
		return (uint32_t)osError;
	}
	if ((options & osFlagsNoClear) != 0U) {
		clear_option = ((options & osFlagsWaitAll) != 0U) ? TX_AND
								  : TX_OR;
	} else {
		clear_option = ((options & osFlagsWaitAll) != 0U)
			       ? TX_AND_CLEAR : TX_OR_CLEAR;
	}
	if (tx_event_flags_get(&slot->group, (ULONG)flags, clear_option,
			       &actual, to_ticks(timeout)) != TX_SUCCESS) {
		return (timeout == 0U) ? (uint32_t)osErrorResource
				       : (uint32_t)osErrorTimeout;
	}
	return (uint32_t)actual;
}

osStatus_t osEventFlagsDelete(osEventFlagsId_t ef_id)
{
	eflags_slot_t *slot = (eflags_slot_t *)ef_id;

	if (ef_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	(void)tx_event_flags_delete(&slot->group);
	slot->state = SLOT_FREE;
	return osOK;
}

/* --- mutexes --------------------------------------------------------------- */

#define MAX_MUTEXES	16

typedef struct {
	TX_MUTEX     mutex;
	slot_state_t state;
} mutex_slot_t;

static mutex_slot_t mutex_slots[MAX_MUTEXES];

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
	mutex_slot_t *slot = NULL;
	UINT inherit = TX_NO_INHERIT;
	uint32_t i;

	if (attr != NULL) {
		if (attr->cb_mem != NULL) {
			return NULL;	/* caller block would be ignored */
		}
		if ((attr->attr_bits & osMutexRecursive) != 0U) {
			/* ThreadX mutexes are not recursive. Accepting the
			 * attribute and delivering a non-recursive mutex
			 * would turn the caller's second lock into a
			 * self-deadlock - refuse instead. */
			return NULL;
		}
		if ((attr->attr_bits & osMutexPrioInherit) != 0U) {
			inherit = TX_INHERIT;
		}
	}

	for (i = 0U; i < MAX_MUTEXES; i++) {
		if (mutex_slots[i].state == SLOT_FREE) {
			slot = &mutex_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		return NULL;
	}

	if (kernel_objects_live) {
		if (tx_mutex_create(&slot->mutex, (CHAR *)"cmsis-mtx",
				    inherit) != TX_SUCCESS) {
			return NULL;
		}
		slot->state = SLOT_LIVE;
		return (osMutexId_t)slot;
	}

	slot->state = SLOT_PENDING;
	if (!defer(DEF_MUTEX, slot, (uintptr_t)inherit, 0, 0, 0)) {
		slot->state = SLOT_FREE;
		return NULL;
	}
	return (osMutexId_t)slot;
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
	mutex_slot_t *slot = (mutex_slot_t *)mutex_id;

	if (mutex_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	return wait_result(tx_mutex_get(&slot->mutex, to_ticks(timeout)),
			   timeout);
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
	mutex_slot_t *slot = (mutex_slot_t *)mutex_id;

	if (mutex_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	if (tx_mutex_put(&slot->mutex) != TX_SUCCESS) {
		return osErrorResource;
	}
	return osOK;
}

osThreadId_t osMutexGetOwner(osMutexId_t mutex_id)
{
	mutex_slot_t *slot = (mutex_slot_t *)mutex_id;
	TX_THREAD *owner = NULL;

	if (mutex_id == NULL || slot->state != SLOT_LIVE) {
		return NULL;
	}
	(void)tx_mutex_info_get(&slot->mutex, NULL, NULL, &owner,
				NULL, NULL, NULL);
	return (osThreadId_t)owner;
}

osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
	mutex_slot_t *slot = (mutex_slot_t *)mutex_id;

	if (mutex_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	(void)tx_mutex_delete(&slot->mutex);
	slot->state = SLOT_FREE;
	return osOK;
}

/* --- semaphores ------------------------------------------------------------ */

#define MAX_SEMAPHORES	32

typedef struct {
	TX_SEMAPHORE sem;
	slot_state_t state;
} sem_slot_t;

static sem_slot_t sem_slots[MAX_SEMAPHORES];

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
			       const osSemaphoreAttr_t *attr)
{
	sem_slot_t *slot = NULL;
	uint32_t i;

	(void)max_count;	/* ThreadX semaphores have no ceiling; the
				 * limit is unenforceable, so it is not
				 * pretended to be enforced. */
	if (attr != NULL && attr->cb_mem != NULL) {
		return NULL;
	}

	for (i = 0U; i < MAX_SEMAPHORES; i++) {
		if (sem_slots[i].state == SLOT_FREE) {
			slot = &sem_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		return NULL;
	}

	if (kernel_objects_live) {
		if (tx_semaphore_create(&slot->sem, (CHAR *)"cmsis-sem",
					(UINT)initial_count) != TX_SUCCESS) {
			return NULL;
		}
		slot->state = SLOT_LIVE;
		return (osSemaphoreId_t)slot;
	}

	slot->state = SLOT_PENDING;
	if (!defer(DEF_SEMAPHORE, slot, (uintptr_t)initial_count, 0, 0, 0)) {
		slot->state = SLOT_FREE;
		return NULL;
	}
	return (osSemaphoreId_t)slot;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
	sem_slot_t *slot = (sem_slot_t *)semaphore_id;

	if (semaphore_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	return wait_result(tx_semaphore_get(&slot->sem, to_ticks(timeout)),
			   timeout);
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
	sem_slot_t *slot = (sem_slot_t *)semaphore_id;

	if (semaphore_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	/* tx_semaphore_put is ISR-callable. */
	if (tx_semaphore_put(&slot->sem) != TX_SUCCESS) {
		return osErrorResource;
	}
	return osOK;
}

uint32_t osSemaphoreGetCount(osSemaphoreId_t semaphore_id)
{
	sem_slot_t *slot = (sem_slot_t *)semaphore_id;
	ULONG current = 0U;

	if (semaphore_id == NULL || slot->state != SLOT_LIVE) {
		return 0U;
	}
	(void)tx_semaphore_info_get(&slot->sem, NULL, &current,
				    NULL, NULL, NULL);
	return (uint32_t)current;
}

osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)
{
	sem_slot_t *slot = (sem_slot_t *)semaphore_id;

	if (semaphore_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	(void)tx_semaphore_delete(&slot->sem);
	slot->state = SLOT_FREE;
	return osOK;
}

/* --- message queues -------------------------------------------------------- */

#define MAX_QUEUES	16

typedef struct {
	TX_QUEUE     queue;
	ULONG        storage[64 * 4];	/* up to 64 messages of 16 bytes */
	uint32_t     msg_size;		/* bytes, as the caller asked */
	slot_state_t state;
} queue_slot_t;

static queue_slot_t queue_slots[MAX_QUEUES];

/* ThreadX queues copy in ULONG words. Round the byte size up; the padding
 * bytes are copied from/into the caller's buffer on Put/Get, so callers
 * must tolerate up to 3 bytes of slack - true of every in-tree caller
 * (sizes are word multiples). msg_prio has no ThreadX equivalent: queues
 * are FIFO, same as the FreeRTOS twin. */
static UINT queue_words(uint32_t msg_size)
{
	uint32_t words = (msg_size + sizeof(ULONG) - 1U) / sizeof(ULONG);

	if (words == 0U) {
		words = 1U;
	}
	if (words > 4U) {
		words = 4U;	/* storage sized for 64 messages of 4 words */
	}
	return (UINT)words;
}

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
				     const osMessageQueueAttr_t *attr)
{
	queue_slot_t *slot = NULL;
	UINT words = queue_words(msg_size);
	uint32_t i;

	(void)attr;
	if (msg_count == 0U || msg_size == 0U || msg_count > 64U) {
		return NULL;
	}

	for (i = 0U; i < MAX_QUEUES; i++) {
		if (queue_slots[i].state == SLOT_FREE) {
			slot = &queue_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		return NULL;
	}

	if (kernel_objects_live) {
		if (tx_queue_create(&slot->queue, (CHAR *)"cmsis-q", words,
				    slot->storage,
				    (ULONG)words * (ULONG)msg_count *
				    sizeof(ULONG)) != TX_SUCCESS) {
			return NULL;
		}
		slot->msg_size = msg_size;
		slot->state = SLOT_LIVE;
		return (osMessageQueueId_t)slot;
	}

	slot->state = SLOT_PENDING;
	if (!defer(DEF_QUEUE, slot, (uintptr_t)msg_count,
		   (uintptr_t)msg_size, (uintptr_t)words, 0)) {
		slot->state = SLOT_FREE;
		return NULL;
	}
	/* Recorded now so GetMsgSize answers even between handle handout and
	 * replay. */
	slot->msg_size = msg_size;
	return (osMessageQueueId_t)slot;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
			     uint8_t msg_prio, uint32_t timeout)
{
	queue_slot_t *slot = (queue_slot_t *)mq_id;

	(void)msg_prio;
	if (mq_id == NULL || msg_ptr == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	return wait_result(tx_queue_send(&slot->queue, (VOID *)msg_ptr,
					 to_ticks(timeout)), timeout);
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
			     uint8_t *msg_prio, uint32_t timeout)
{
	queue_slot_t *slot = (queue_slot_t *)mq_id;

	if (msg_prio != NULL) {
		*msg_prio = 0U;
	}
	if (mq_id == NULL || msg_ptr == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	return wait_result(tx_queue_receive(&slot->queue, msg_ptr,
					    to_ticks(timeout)), timeout);
}

uint32_t osMessageQueueGetMsgSize(osMessageQueueId_t mq_id)
{
	queue_slot_t *slot = (queue_slot_t *)mq_id;

	if (mq_id == NULL || slot->state != SLOT_LIVE) {
		return 0U;
	}
	return slot->msg_size;
}

uint32_t osMessageQueueGetCount(osMessageQueueId_t mq_id)
{
	queue_slot_t *slot = (queue_slot_t *)mq_id;
	ULONG queued = 0U;

	if (mq_id == NULL || slot->state != SLOT_LIVE) {
		return 0U;
	}
	(void)tx_queue_info_get(&slot->queue, NULL, &queued, NULL,
				NULL, NULL, NULL);
	return (uint32_t)queued;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
	queue_slot_t *slot = (queue_slot_t *)mq_id;

	if (mq_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	(void)tx_queue_delete(&slot->queue);
	slot->state = SLOT_FREE;
	return osOK;
}

/* --- timers ---------------------------------------------------------------- */

#define MAX_TIMERS	16

typedef struct {
	TX_TIMER     timer;
	osTimerFunc_t func;
	void        *arg;
	uint8_t      periodic;
	slot_state_t state;
} timer_slot_t;

static timer_slot_t timer_slots[MAX_TIMERS];

/* Runs in the kernel's timer thread - the same context discipline as the
 * FreeRTOS image's timer task. */
static void timer_trampoline(ULONG argument)
{
	timer_slot_t *slot = (timer_slot_t *)(uintptr_t)argument;

	if (slot->func != NULL) {
		slot->func(slot->arg);
	}
}

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type,
		       void *argument, const osTimerAttr_t *attr)
{
	timer_slot_t *slot = NULL;
	uint32_t i;

	(void)attr;
	if (func == NULL) {
		return NULL;
	}

	for (i = 0U; i < MAX_TIMERS; i++) {
		if (timer_slots[i].state == SLOT_FREE) {
			slot = &timer_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		return NULL;
	}

	slot->func = func;
	slot->arg = argument;
	slot->periodic = (type == osTimerPeriodic) ? 1U : 0U;

	if (kernel_objects_live) {
		if (tx_timer_create(&slot->timer, (CHAR *)"cmsis-tmr",
				    timer_trampoline,
				    (ULONG)(uintptr_t)slot,
				    1U, slot->periodic ? 1U : 0U,
				    TX_NO_ACTIVATE) != TX_SUCCESS) {
			return NULL;
		}
		slot->state = SLOT_LIVE;
		return (osTimerId_t)slot;
	}

	slot->state = SLOT_PENDING;
	if (!defer(DEF_TIMER, slot, (uintptr_t)slot->periodic, 0, 0, 0)) {
		slot->state = SLOT_FREE;
		return NULL;
	}
	return (osTimerId_t)slot;
}

osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks)
{
	timer_slot_t *slot = (timer_slot_t *)timer_id;
	UINT status;

	if (timer_id == NULL || ticks == 0U || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	(void)tx_timer_deactivate(&slot->timer);
	/* tx_timer_change sets BOTH the first period and the reschedule
	 * period, which is exactly CMSIS's osTimerStart contract. */
	status = tx_timer_change(&slot->timer, (ULONG)ticks,
				 slot->periodic ? (ULONG)ticks : 0U);
	if (status != TX_SUCCESS) {
		return osErrorParameter;
	}
	if (tx_timer_activate(&slot->timer) != TX_SUCCESS) {
		return osErrorResource;
	}
	return osOK;
}

osStatus_t osTimerStop(osTimerId_t timer_id)
{
	timer_slot_t *slot = (timer_slot_t *)timer_id;

	if (timer_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	if (tx_timer_deactivate(&slot->timer) != TX_SUCCESS) {
		return osErrorResource;
	}
	return osOK;
}

osStatus_t osTimerDelete(osTimerId_t timer_id)
{
	timer_slot_t *slot = (timer_slot_t *)timer_id;

	if (timer_id == NULL || slot->state != SLOT_LIVE) {
		return osErrorParameter;
	}
	(void)tx_timer_deactivate(&slot->timer);
	if (tx_timer_delete(&slot->timer) != TX_SUCCESS) {
		return osErrorParameter;
	}
	slot->func = NULL;
	slot->state = SLOT_FREE;
	return osOK;
}

/* --- memory pools ---------------------------------------------------------- */
/* Same code as the FreeRTOS twin: a fixed-block free list over one region.
 * No kernel primitives involved, so the two implementations are identical
 * by construction (both kernel-neutral, both static). */

typedef struct pool_block {
	struct pool_block *next;
} pool_block_t;

typedef struct {
	uint32_t      block_size;
	uint32_t      block_count;
	uint32_t      used;
	pool_block_t *free_list;
	void         *storage;
	uint8_t       storage_is_ours;
} mem_pool_t;

#define MAX_POOLS	8
#define POOL_STORAGE_BYTES	512u

static mem_pool_t pools[MAX_POOLS];
static uint8_t pool_storage[MAX_POOLS][POOL_STORAGE_BYTES];

osMemoryPoolId_t osMemoryPoolNew(uint32_t block_count, uint32_t block_size,
				 const osMemoryPoolAttr_t *attr)
{
	uint32_t i;
	uint32_t stride;
	uint8_t *p;

	if (block_count == 0U || block_size == 0U) {
		return NULL;
	}
	if (attr != NULL && attr->cb_mem != NULL) {
		return NULL;
	}
	stride = (block_size + sizeof(void *) - 1U) &
		 ~(uint32_t)(sizeof(void *) - 1U);

	for (i = 0U; i < MAX_POOLS; i++) {
		if (pools[i].block_size == 0U) {
			break;
		}
	}
	if (i == MAX_POOLS) {
		return NULL;
	}
	if (attr != NULL && attr->mp_mem != NULL) {
		p = (uint8_t *)attr->mp_mem;
		pools[i].storage_is_ours = 0U;
	} else {
		if (stride * block_count > POOL_STORAGE_BYTES) {
			return NULL;
		}
		p = pool_storage[i];
		pools[i].storage_is_ours = 1U;
	}

	pools[i].block_size = stride;
	pools[i].block_count = block_count;
	pools[i].used = 0U;
	pools[i].storage = p;
	pools[i].free_list = NULL;

	{
		uint32_t k;

		for (k = block_count; k > 0U; k--) {
			pool_block_t *blk =
				(pool_block_t *)(p + (k - 1U) * stride);

			blk->next = pools[i].free_list;
			pools[i].free_list = blk;
		}
	}
	return (osMemoryPoolId_t)&pools[i];
}

void *osMemoryPoolAlloc(osMemoryPoolId_t mp_id, uint32_t timeout)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;
	pool_block_t *blk;

	(void)timeout;	/* never blocks: has a block or does not */
	if (pool == NULL || pool->free_list == NULL) {
		return NULL;
	}
	blk = pool->free_list;
	pool->free_list = blk->next;
	pool->used++;
	return (void *)blk;
}

osStatus_t osMemoryPoolFree(osMemoryPoolId_t mp_id, void *block)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;
	pool_block_t *blk = (pool_block_t *)block;

	if (pool == NULL || block == NULL || pool->used == 0U) {
		return osErrorParameter;
	}
	blk->next = pool->free_list;
	pool->free_list = blk;
	pool->used--;
	return osOK;
}

uint32_t osMemoryPoolGetCount(osMemoryPoolId_t mp_id)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;

	return (pool == NULL) ? 0U : pool->used;
}

osStatus_t osMemoryPoolDelete(osMemoryPoolId_t mp_id)
{
	mem_pool_t *pool = (mem_pool_t *)mp_id;

	if (pool == NULL) {
		return osErrorParameter;
	}
	pool->block_size = 0U;
	pool->block_count = 0U;
	pool->used = 0U;
	pool->free_list = NULL;
	return osOK;
}

/* --- deferred-construction replay ------------------------------------------ */
/* Called by tx_application_define (tx_glue.c): the one sanctioned window
 * where ThreadX objects may be created before the scheduler runs. */

void tx_cmsis_application_define(void *first_unused_memory)
{
	uint32_t i;

	(void)first_unused_memory;
	kernel_objects_live = 1U;

	for (i = 0U; i < deferral_count; i++) {
		def_record_t *d = &deferrals[i];

		switch (d->kind) {
		case DEF_THREAD: {
			thread_slot_t *slot = (thread_slot_t *)d->slot;

			thread_create_into_slot(slot,
						(osThreadFunc_t)d->a0,
						(void *)d->a1, (UINT)d->a2,
						(uint32_t)d->a3);
			break;
		}
		case DEF_EVENT_FLAGS: {
			eflags_slot_t *slot = (eflags_slot_t *)d->slot;

			if (tx_event_flags_create(&slot->group,
						  (CHAR *)"cmsis-ef") ==
			    TX_SUCCESS) {
				slot->state = SLOT_LIVE;
			} else {
				slot->state = SLOT_FREE;
			}
			break;
		}
		case DEF_SEMAPHORE: {
			sem_slot_t *slot = (sem_slot_t *)d->slot;

			if (tx_semaphore_create(&slot->sem, (CHAR *)"cmsis-sem",
						(UINT)d->a0) == TX_SUCCESS) {
				slot->state = SLOT_LIVE;
			} else {
				slot->state = SLOT_FREE;
			}
			break;
		}
		case DEF_MUTEX: {
			mutex_slot_t *slot = (mutex_slot_t *)d->slot;

			if (tx_mutex_create(&slot->mutex, (CHAR *)"cmsis-mtx",
					    (UINT)d->a0) == TX_SUCCESS) {
				slot->state = SLOT_LIVE;
			} else {
				slot->state = SLOT_FREE;
			}
			break;
		}
		case DEF_QUEUE: {
			queue_slot_t *slot = (queue_slot_t *)d->slot;

			if (tx_queue_create(&slot->queue, (CHAR *)"cmsis-q",
					    (UINT)d->a2, slot->storage,
					    (ULONG)d->a2 * (ULONG)d->a0 *
					    sizeof(ULONG)) == TX_SUCCESS) {
				slot->msg_size = (uint32_t)d->a1;
				slot->state = SLOT_LIVE;
			} else {
				slot->state = SLOT_FREE;
			}
			break;
		}
		case DEF_TIMER: {
			timer_slot_t *slot = (timer_slot_t *)d->slot;
			UINT resched = d->a0 ? 1U : 0U;

			if (tx_timer_create(&slot->timer, (CHAR *)"cmsis-tmr",
					    timer_trampoline,
					    (ULONG)(uintptr_t)slot,
					    1U, resched,
					    TX_NO_ACTIVATE) == TX_SUCCESS) {
				slot->state = SLOT_LIVE;
			} else {
				slot->state = SLOT_FREE;
			}
			break;
		}
		default:
			break;
		}
	}
	deferral_count = 0U;
}
