/*
 * @file   netutils_shim.c
 * @brief  RT-Thread API realizations for the vendored netutils sources.
 *
 * See shim/rtthread.h for the mapping table. Two facts this file is built
 * around:
 *
 *  - tick parity: the ThreadX tick is 1 ms (tx_user.h), which matches
 *    RT-Thread's RT_TICK_PER_SECOND=1000, so tick values cross the shim
 *    unchanged (delays, tick-difference timing).
 *  - output routing: rt_kprintf and the LOG_* macros must land on the shell
 *    session that ran the command, not always on the UART. A session binds
 *    its shell task's thread id (netutils_shim_bind_session); output from a
 *    bound thread goes to that session's csh_printf, everything else goes
 *    to the locked console sink. The map is written once at session setup
 *    (thread context) and read per output call under a short critical
 *    section - the telnet feat is what fills it.
 */

#include <rtthread.h>
#include <tx_api.h>
#include <cmsis_os2.h>
#include "board.h"
#include "csh.h"

/* --- session-routed output ------------------------------------------------- */

#define NETUTILS_SESSION_MAX	4

struct netutils_session {
	void *thread;		/* osThreadId_t of the shell task */
	void *csh;		/* chry_shell_t of the session */
};

static struct netutils_session sessions[NETUTILS_SESSION_MAX];

void netutils_shim_bind_session(void *csh)
{
	rt_base_t level = rt_hw_interrupt_disable();
	void *self = osThreadGetId();
	uint32_t i;

	for (i = 0; i < NETUTILS_SESSION_MAX; i++) {
		if (sessions[i].thread == self) {
			sessions[i].csh = csh;
			break;
		}
	}
	if (i == NETUTILS_SESSION_MAX) {
		for (i = 0; i < NETUTILS_SESSION_MAX; i++) {
			if (sessions[i].thread == NULL) {
				sessions[i].thread = self;
				sessions[i].csh = csh;
				break;
			}
		}
	}
	rt_hw_interrupt_enable(level);
}

void netutils_shim_unbind_session(void)
{
	rt_base_t level = rt_hw_interrupt_disable();
	void *self = osThreadGetId();
	uint32_t i;

	for (i = 0; i < NETUTILS_SESSION_MAX; i++) {
		if (sessions[i].thread == self) {
			sessions[i].thread = NULL;
			sessions[i].csh = NULL;
		}
	}
	rt_hw_interrupt_enable(level);
}

static void netutils_output(const char *buf)
{
	rt_base_t level = rt_hw_interrupt_disable();
	void *self = osThreadGetId();
	void *csh = NULL;
	uint32_t i;

	for (i = 0; i < NETUTILS_SESSION_MAX; i++) {
		if (sessions[i].thread == self) {
			csh = sessions[i].csh;
			break;
		}
	}
	rt_hw_interrupt_enable(level);

	if (csh != NULL) {
		csh_printf(csh, "%s", buf);
	} else {
		board_console_write(buf);
	}
}

int rt_kprintf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	netutils_output(buf);
	return n;
}

void netutils_shim_log(const char *level, const char *fmt, ...)
{
	char buf[280];
	size_t off;
	va_list ap;

	va_start(ap, fmt);
	off = (size_t)snprintf(buf, sizeof(buf), "[%s/netutils] ", level);
	(void)vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
	va_end(ap);
	netutils_output(buf);
}

/* --- heap ------------------------------------------------------------------ */

/* The libc names resolve to the image's TLSF-backed malloc family (the
 * libbsd adapter's host-world libc unit provides them for the whole
 * image); no second allocator is created here - cross-freeing between the
 * netutils world and the rest of the tree must stay safe. */

void *rt_malloc(rt_size_t size)
{
	return malloc(size);
}

void rt_free(void *ptr)
{
	free(ptr);
}

void *rt_calloc(rt_size_t count, rt_size_t size)
{
	return calloc(count, size);
}

void *rt_realloc(void *ptr, rt_size_t size)
{
	return realloc(ptr, size);
}

char *rt_strdup(const char *s)
{
	size_t len;
	char *copy;

	if (s == NULL) {
		return NULL;
	}
	len = strlen(s) + 1U;
	copy = malloc(len);
	if (copy != NULL) {
		memcpy(copy, s, len);
	}
	return copy;
}

/* --- str/mem realizations --------------------------------------------------
 *
 * The macros in rtthread.h serve TUs that include the shim header (ping.c);
 * the tftp core is deliberately RTOS-free and calls the rt_mem... and
 * rt_sprintf names bare, so they must also exist as functions. #undef first:
 * this file itself includes rtthread.h and the macros would otherwise
 * rewrite these definitions into libc-name collisions.
 */

#undef rt_memset
#undef rt_memcpy
#undef rt_memmove
#undef rt_memcmp
#undef rt_strcpy
#undef rt_strcat
#undef rt_strlen

void *rt_memset(void *s, int c, rt_size_t n)
{
	return memset(s, c, n);
}

void *rt_memcpy(void *dest, const void *src, rt_size_t n)
{
	return memcpy(dest, src, n);
}

void *rt_memmove(void *dest, const void *src, rt_size_t n)
{
	return memmove(dest, src, n);
}

int rt_memcmp(const void *a, const void *b, rt_size_t n)
{
	return memcmp(a, b, n);
}

char *rt_strcpy(char *dest, const char *src)
{
	return strcpy(dest, src);
}

char *rt_strcat(char *dest, const char *src)
{
	return strcat(dest, src);
}

rt_size_t rt_strlen(const char *s)
{
	return strlen(s);
}

int rt_sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	/* unbounded by contract; the bound only stops a runaway format from
	 * running off the image - callers pass paths and short labels */
	n = vsnprintf(buf, 4096u, fmt, ap);
	va_end(ap);
	return n;
}

/* --- tick ------------------------------------------------------------------ */

rt_tick_t rt_tick_get(void)
{
	return (rt_tick_t)tx_time_get();
}

rt_tick_t rt_tick_from_millisecond(rt_int32_t ms)
{
	/* tick == 1 ms; negative means "forever" on RT-Thread and osWaitForever
	 * (-1 in CMSIS value space) means the same here. */
	return (ms < 0) ? (rt_tick_t)RT_WAITING_FOREVER : (rt_tick_t)ms;
}

/* --- threads ----------------------------------------------------------------- */

#define NETUTILS_THREAD_MAX	8

struct netutils_thread {
	uint8_t in_use;
	uint8_t started;
	const char *name;
	osThreadFunc_t entry;
	void *param;
	uint32_t stack_size;
	rt_uint8_t priority;
	osThreadId_t tid;
};

static struct netutils_thread threads[NETUTILS_THREAD_MAX];

/* RT-Thread priorities count DOWN (0 is highest); the CMSIS bands count up.
 * The ladder this image relies on is shell(5) > tcpip(4) >= supplicant(4) >
 * usbdi worker(<=3): helper threads spawned by netutils tools land at band 3
 * (osPriorityNormal) so they can never starve tcpip, regardless of the
 * RT-Thread priority the vendored code asked for. */
static osPriority_t netutils_prio(rt_uint8_t rt_prio)
{
	(void)rt_prio;
	return osPriorityNormal;
}

rt_thread_t rt_thread_create(const char *name,
			     void (*entry)(void *parameter), void *parameter,
			     rt_uint32_t stack_size, rt_uint8_t priority,
			     rt_uint32_t tick)
{
	uint32_t i;

	(void)tick;
	for (i = 0; i < NETUTILS_THREAD_MAX; i++) {
		if (!threads[i].in_use) {
			threads[i].in_use = 1;
			threads[i].started = 0;
			threads[i].name = name;
			threads[i].entry = (osThreadFunc_t)entry;
			threads[i].param = parameter;
			threads[i].stack_size = stack_size;
			threads[i].priority = priority;
			threads[i].tid = NULL;
			return &threads[i];
		}
	}
	return NULL;
}

rt_err_t rt_thread_startup(rt_thread_t thread)
{
	struct netutils_thread *t = (struct netutils_thread *)thread;
	osThreadAttr_t attr;

	if (t == NULL || !t->in_use || t->started) {
		return -RT_ERROR;
	}
	attr.name = t->name;
	attr.attr_bits = 0;
	attr.cb_mem = NULL;
	attr.cb_size = 0;
	attr.stack_mem = NULL;
	attr.stack_size = t->stack_size;
	attr.priority = netutils_prio(t->priority);
	attr.tz_module = 0;

	t->tid = osThreadNew(t->entry, t->param, &attr);
	t->started = (t->tid != NULL);
	return (t->tid != NULL) ? RT_EOK : -RT_ERROR;
}

rt_thread_t rt_thread_self(void)
{
	return (rt_thread_t)osThreadGetId();
}

rt_err_t rt_thread_delay(rt_tick_t tick)
{
	return (osDelay((uint32_t)tick) == osOK) ? RT_EOK : -RT_ERROR;
}

rt_err_t rt_thread_mdelay(rt_int32_t ms)
{
	return (osDelay((uint32_t)ms) == osOK) ? RT_EOK : -RT_ERROR;
}

/* --- IPC ----------------------------------------------------------------------- */

static uint32_t netutils_wait(rt_int32_t time)
{
	return (time == RT_WAITING_FOREVER) ? (uint32_t)osWaitForever
					    : (uint32_t)time;
}

rt_sem_t rt_sem_create(const char *name, void *priv, rt_uint8_t value,
		       rt_uint8_t flag)
{
	(void)name;
	(void)priv;
	(void)flag;
	/* ThreadX semaphores have no ceiling; the port ignores max_count. */
	return (rt_sem_t)osSemaphoreNew(0xFFFFFFFFu, value, NULL);
}

rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time)
{
	return (osSemaphoreAcquire((osSemaphoreId_t)sem,
				   netutils_wait(time)) == osOK)
		       ? RT_EOK
		       : -RT_ERROR;
}

rt_err_t rt_sem_release(rt_sem_t sem)
{
	return (osSemaphoreRelease((osSemaphoreId_t)sem) == osOK)
		       ? RT_EOK
		       : -RT_ERROR;
}

rt_err_t rt_sem_delete(rt_sem_t sem)
{
	return (osSemaphoreDelete((osSemaphoreId_t)sem) == osOK)
		       ? RT_EOK
		       : -RT_ERROR;
}

rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag)
{
	osMutexAttr_t attr;

	(void)flag;
	attr.name = name;
	attr.attr_bits = 0;	/* non-recursive, priority inherit */
	attr.cb_mem = NULL;
	attr.cb_size = 0;
	return (rt_mutex_t)osMutexNew(&attr);
}

rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time)
{
	return (osMutexAcquire((osMutexId_t)mutex,
			       netutils_wait(time)) == osOK)
		       ? RT_EOK
		       : -RT_ERROR;
}

rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
	return (osMutexRelease((osMutexId_t)mutex) == osOK) ? RT_EOK
							    : -RT_ERROR;
}

rt_err_t rt_mutex_delete(rt_mutex_t mutex)
{
	return (osMutexDelete((osMutexId_t)mutex) == osOK) ? RT_EOK
							   : -RT_ERROR;
}

rt_mailbox_t rt_mb_create(const char *name, rt_size_t size, rt_uint8_t flag)
{
	(void)name;
	(void)flag;
	/* RT mailboxes carry one word per message; queues copy ULONG words
	 * (the port rounds msg_size up). */
	return (rt_mailbox_t)osMessageQueueNew((uint32_t)size,
					       sizeof(uintptr_t), NULL);
}

rt_err_t rt_mb_send(rt_mailbox_t mb, rt_ubase_t value)
{
	uintptr_t v = (uintptr_t)value;

	return (osMessageQueuePut((osMessageQueueId_t)mb, &v, 0, 0) == osOK)
		       ? RT_EOK
		       : -RT_ERROR;
}

rt_err_t rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t time)
{
	uintptr_t v;

	if (osMessageQueueGet((osMessageQueueId_t)mb, &v, NULL,
			      netutils_wait(time)) != osOK) {
		return -RT_ERROR;
	}
	*value = (rt_ubase_t)v;
	return RT_EOK;
}

rt_err_t rt_mb_delete(rt_mailbox_t mb)
{
	return (osMessageQueueDelete((osMessageQueueId_t)mb) == osOK)
		       ? RT_EOK
		       : -RT_ERROR;
}

/* --- wall clock (RAM epoch, no RTC on this trunk) --------------------------- */

/* The synced wall clock is a UNIX-epoch value anchored at the tick it was
 * set at; elapsed ticks advance it. 0 means "never synced" (tick 0 would be
 * indistinguishable anyway this early in the boot). */
static long ntp_epoch;
static rt_tick_t ntp_epoch_tick;

void netutils_ntp_set_epoch(long unix_sec)
{
	rt_base_t level = rt_hw_interrupt_disable();

	ntp_epoch = unix_sec;
	ntp_epoch_tick = tx_time_get();
	rt_hw_interrupt_enable(level);
}

long netutils_ntp_get_epoch(void)
{
	rt_base_t level = rt_hw_interrupt_disable();
	long now = ntp_epoch;
	rt_tick_t anchor = ntp_epoch_tick;

	rt_hw_interrupt_enable(level);
	if (now == 0) {
		return 0;
	}
	return now + (long)((rt_tick_t)tx_time_get() - anchor) / 1000;
}

/* --- interrupts ------------------------------------------------------------------- */

rt_base_t rt_hw_interrupt_disable(void)
{
	return (rt_base_t)tx_interrupt_control(TX_INT_DISABLE);
}

void rt_hw_interrupt_enable(rt_base_t level)
{
	tx_interrupt_control((UINT)level);
}
