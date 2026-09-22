/*
 * Copyright (c) 2024, sakumisu
 * Copyright (c) 2026, zhugengyu (integration adaptations, below)
 * SPDX-License-Identifier: Apache-2.0
 *
 * CherryUSB's ThreadX osal, whole-file ported from upstream osal/
 * usb_osal_threadx.c (master 0e40349b) with three integration
 * adaptations, registered in imports.md:
 *
 * 1. The byte pool is ours. Upstream externs an integrator-created
 *    usb_byte_pool; here it is a static .bss region and usb_osal_init
 *    (called once from the usb start task) creates the pool over it -
 *    same shape, no board header plumbing. The FreeRTOS line's osal
 *    needs no init at all, so the call sits behind THREADX_BUILD in
 *    usbh_adapter.c, next to the kernel banner in main.c.
 *
 * 2. Priorities pass through raw. CherryUSB numbers 0 as most urgent;
 *    ThreadX agrees (0 = highest urgency), so unlike the FreeRTOS osal
 *    no inversion is needed. The PSC hub threads run at priority 0 -
 *    they sleep on their message queue between events, exactly the
 *    trade the FreeRTOS line already accepted.
 *
 * 3. TX_TIMER_TICKS_PER_SECOND must say 1000: our tick glue arms the
 *    1 kHz timer, and usb_osal_msleep refuses to compile against any
 *    other claim (upstream added that #error; tx_user.h defines it).
 */

#include "usb_osal.h"
#include "usb_errno.h"
#include "usb_config.h"
#include "usb_util.h"		/* USB_ALIGN_UP (usb_def.h clashes on __PACKED here) */
#include "usb_log.h"
#include "tx_api.h"

/* Sized for two hub threads (TCB + 64 KiB stack each - the net80211
 * attach chain runs there and is the deepest call stack in the image),
 * the reaper, the EHCI descriptor pools and the core's per-device
 * allocations, with room to spare. */
#define USB_OSAL_POOL_BYTES	(512u * 1024u)

static uint8_t usb_osal_pool_mem[USB_OSAL_POOL_BYTES]
	__attribute__((aligned(16)));
static TX_BYTE_POOL usb_byte_pool;
static uint8_t usb_osal_started;

usb_osal_mq_t usb_osal_mq;

usb_osal_thread_t usb_osal_thread_create(const char *name, uint32_t stack_size, uint32_t prio, usb_thread_entry_t entry, void *args)
{
    TX_THREAD *thread_ptr = TX_NULL;

    tx_byte_allocate(&usb_byte_pool, (VOID **)&thread_ptr, USB_ALIGN_UP(sizeof(TX_THREAD), 4) + stack_size, TX_NO_WAIT);
    if (thread_ptr == TX_NULL) {
        USB_LOG_ERR("Create thread %s failed\r\n", name);
        while (1) {
        }
    }

    tx_thread_create(thread_ptr, (CHAR *)name, (VOID(*)(ULONG))entry, (uintptr_t)args,
                     (VOID *)thread_ptr + USB_ALIGN_UP(sizeof(TX_THREAD), 4), stack_size,
                     prio, prio, TX_NO_TIME_SLICE, TX_AUTO_START);

    return (usb_osal_thread_t)thread_ptr;
}

void usb_osal_thread_delete(usb_osal_thread_t thread)
{
    if (thread == NULL) {
        thread = tx_thread_identify();

        usb_osal_mq_send(usb_osal_mq, (uintptr_t)thread);

        tx_thread_terminate(thread);
        return;
    }

    tx_thread_terminate(thread);
    tx_thread_delete(thread);
    tx_byte_release(thread);
}

void usb_osal_thread_schedule_other(void)
{
    TX_THREAD *current_thread = tx_thread_identify();
    const UINT old_priority = current_thread->tx_thread_priority;

    tx_thread_priority_change(current_thread, TX_MAX_PRIORITIES - 1, &old_priority);

    tx_thread_relinquish();

    tx_thread_priority_change(current_thread, old_priority, &old_priority);
}

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
{
    TX_SEMAPHORE *sem_ptr = TX_NULL;

    tx_byte_allocate(&usb_byte_pool, (VOID **)&sem_ptr, sizeof(TX_SEMAPHORE), TX_NO_WAIT);
    if (sem_ptr == TX_NULL) {
        USB_LOG_ERR("Create semaphore failed\r\n");
        while (1) {
        }
    }

    tx_semaphore_create(sem_ptr, "usbh_sem", initial_count);
    return (usb_osal_sem_t)sem_ptr;
}

usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count)
{
    return usb_osal_sem_create(0);
}

void usb_osal_sem_delete(usb_osal_sem_t sem)
{
    tx_semaphore_delete((TX_SEMAPHORE *)sem);
    tx_byte_release(sem);
}

int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    int ret = 0;

    ret = tx_semaphore_get((TX_SEMAPHORE *)sem, timeout);
    if (ret == TX_SUCCESS) {
        ret = 0;
    } else if ((ret == TX_WAIT_ABORTED) || (ret == TX_NO_INSTANCE)) {
        ret = -USB_ERR_TIMEOUT;
    } else {
        ret = -USB_ERR_INVAL;
    }

    return (int)ret;
}

int usb_osal_sem_give(usb_osal_sem_t sem)
{
    return (tx_semaphore_put((TX_SEMAPHORE *)sem) == TX_SUCCESS) ? 0 : -USB_ERR_INVAL;
}

void usb_osal_sem_reset(usb_osal_sem_t sem)
{
    tx_semaphore_get((TX_SEMAPHORE *)sem, 0);
}

usb_osal_mutex_t usb_osal_mutex_create(void)
{
    TX_MUTEX *mutex_ptr = TX_NULL;

    tx_byte_allocate(&usb_byte_pool, (VOID **)&mutex_ptr, sizeof(TX_MUTEX), TX_NO_WAIT);
    if (mutex_ptr == TX_NULL) {
        return NULL;
    }

    tx_mutex_create(mutex_ptr, "usbh_mutex", TX_INHERIT);
    return (usb_osal_mutex_t)mutex_ptr;
}

void usb_osal_mutex_delete(usb_osal_mutex_t mutex)
{
    tx_mutex_delete((TX_MUTEX *)mutex);
    tx_byte_release(mutex);
}

int usb_osal_mutex_take(usb_osal_mutex_t mutex)
{
    int ret = 0;

    ret = tx_mutex_get((TX_MUTEX *)mutex, TX_WAIT_FOREVER);
    if (ret == TX_SUCCESS) {
        ret = 0;
    } else if ((ret == TX_WAIT_ABORTED) || (ret == TX_NO_INSTANCE)) {
        ret = -USB_ERR_TIMEOUT;
    } else {
        ret = -USB_ERR_INVAL;
    }

    return (int)ret;
}

int usb_osal_mutex_give(usb_osal_mutex_t mutex)
{
    return (tx_mutex_put((TX_MUTEX *)mutex) == TX_SUCCESS) ? 0 : -USB_ERR_INVAL;
}

usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs)
{
    TX_QUEUE *queue_ptr = TX_NULL;

    tx_byte_allocate(&usb_byte_pool, (VOID **)&queue_ptr, USB_ALIGN_UP(sizeof(TX_QUEUE), 4) + sizeof(uintptr_t) * max_msgs, TX_NO_WAIT);
    if (queue_ptr == TX_NULL) {
        return NULL;
    }

    tx_queue_create(queue_ptr, "usbh_mq", sizeof(uintptr_t) / 4, (CHAR *)queue_ptr + USB_ALIGN_UP(sizeof(TX_QUEUE), 4), sizeof(uintptr_t) * max_msgs);
    return (usb_osal_mq_t)queue_ptr;
}

void usb_osal_mq_delete(usb_osal_mq_t mq)
{
    tx_queue_delete((TX_QUEUE *)mq);
    tx_byte_release(mq);
}

int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr)
{
    return (tx_queue_send((TX_QUEUE *)mq, &addr, TX_NO_WAIT) == TX_SUCCESS) ? 0 : -USB_ERR_INVAL;
}

int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout)
{
    int ret = 0;

    ret = tx_queue_receive((TX_QUEUE *)mq, addr, timeout);
    if (ret == TX_SUCCESS) {
        ret = 0;
    } else if (ret == TX_QUEUE_EMPTY) {
        ret = -USB_ERR_TIMEOUT;
    } else {
        ret = -USB_ERR_INVAL;
    }

    return (int)ret;
}

struct usb_osal_timer *usb_osal_timer_create(const char *name, uint32_t timeout_ms, usb_timer_handler_t handler, void *argument, bool is_period)
{
    TX_TIMER *timer_ptr = TX_NULL;
    struct usb_osal_timer *timer;

    tx_byte_allocate(&usb_byte_pool, (VOID **)&timer, sizeof(struct usb_osal_timer), TX_NO_WAIT);
    if (timer == TX_NULL) {
        return NULL;
    }
    memset(timer, 0, sizeof(struct usb_osal_timer));

    tx_byte_allocate(&usb_byte_pool, (VOID **)&timer_ptr, sizeof(TX_TIMER), TX_NO_WAIT);
    if (timer_ptr == TX_NULL) {
        tx_byte_release(timer);
        return NULL;
    }

    timer->timer = timer_ptr;
    timer->timeout_ms = timeout_ms;
    timer->is_period = is_period;
    if (tx_timer_create(timer_ptr, (CHAR *)name, (void (*)(ULONG))handler, (uintptr_t)argument, 1, is_period ? 1 : 0,
                        TX_NO_ACTIVATE) != TX_SUCCESS) {
        tx_byte_release(timer_ptr);
        tx_byte_release(timer);
        return NULL;
    }
    return timer;
}

void usb_osal_timer_delete(struct usb_osal_timer *timer)
{
    tx_timer_deactivate((TX_TIMER *)timer->timer);
    tx_timer_delete((TX_TIMER *)timer->timer);
    tx_byte_release(timer->timer);
    tx_byte_release(timer);
}

void usb_osal_timer_start(struct usb_osal_timer *timer)
{
    if (tx_timer_change((TX_TIMER *)timer->timer, timer->timeout_ms, timer->is_period ? timer->timeout_ms : 0) == TX_SUCCESS) {
        (void)tx_timer_activate((TX_TIMER *)timer->timer);
    }
}

void usb_osal_timer_stop(struct usb_osal_timer *timer)
{
    tx_timer_deactivate((TX_TIMER *)timer->timer);
}

size_t usb_osal_enter_critical_section(void)
{
    TX_INTERRUPT_SAVE_AREA

    TX_DISABLE

    return interrupt_save;
}

void usb_osal_leave_critical_section(size_t flag)
{
    TX_INTERRUPT_SAVE_AREA

    interrupt_save = flag;
    TX_RESTORE
}

void usb_osal_msleep(uint32_t delay)
{
#if TX_TIMER_TICKS_PER_SECOND != 1000
#error "TX_TIMER_TICKS_PER_SECOND must be 1000"
#endif
    tx_thread_sleep(delay);
}

void *usb_osal_malloc(size_t size)
{
    CHAR *pointer = TX_NULL;

    tx_byte_allocate(&usb_byte_pool, (VOID **)&pointer, size, TX_WAIT_FOREVER);

    return pointer;
}

void usb_osal_free(void *ptr)
{
    tx_byte_release(ptr);
}

static void usb_osal_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)
{
    int ret;
    usb_osal_thread_t thread;

    while (1) {
        ret = usb_osal_mq_recv(usb_osal_mq, (uintptr_t *)&thread, TX_WAIT_FOREVER);
        if (ret < 0) {
            continue;
        }
        tx_thread_delete(thread);
        tx_byte_release(thread);
    }
}

/* Creates the byte pool - over the caller's region when given one, over the
 * static region otherwise - then the self-delete reaper thread and its
 * request queue, exactly as upstream. Idempotent so a future second caller
 * is harmless. */
void usb_osal_init(uint8_t *mem, uint32_t mem_size)
{
    usb_osal_thread_t thread;

    if (usb_osal_started != 0U) {
        return;
    }
    if (mem == NULL) {
        mem = usb_osal_pool_mem;
        mem_size = USB_OSAL_POOL_BYTES;
    }
    usb_osal_started = 1U;

    tx_byte_pool_create(&usb_byte_pool, "usb byte pool", mem, mem_size);

    thread = usb_osal_thread_create("usb_osal", 2048, 10, usb_osal_thread, NULL);
    if (thread == NULL) {
        USB_LOG_ERR("Create usb_osal_thread failed\r\n");
        while (1) {
        }
    }

    usb_osal_mq = usb_osal_mq_create(32);
    if (usb_osal_mq == NULL) {
        USB_LOG_ERR("Create usb_osal_mq failed\r\n");
        while (1) {
        }
    }
}
