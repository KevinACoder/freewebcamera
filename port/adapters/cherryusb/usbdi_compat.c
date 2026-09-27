/*
 * @file
 * @brief NetBSD usbd_*(9) driver-facing API implemented over the CherryUSB
 *        host stack.
 *
 * WHAT THIS IS
 *  - usbd_do_request -> usbh_control_transfer (blocking, 500 ms timeout
 *    inside cherryusb, one 64-byte aligned bounce buffer + mutex, three
 *    attempts before the error is reported);
 *  - usbd_transfer -> usbh_submit_urb with urb->timeout = 0, i.e. pure
 *    asynchronous per-URB QH; the NetBSD timeout watchdog is the shim's
 *    own sweep (see below);
 *  - the NetBSD per-pipe FIFO (up_queue) is reproduced: the CherryUSB HCD
 *    keeps one urb per endpoint at a time, later xfers park on the pipe
 *    and the urb worker arms the next in submission order
 *    (usbdi_pipe_kick) - armed urbs beyond the first would be served
 *    newest-first by the EHCI async ring, and the device emits bulk
 *    frames in arrival order;
 *  - urb->complete runs in the EHCI interrupt handler, so it only records
 *    the result and drops the xfer into an ISR-safe ring (store data,
 *    `dmb ish`, store index) plus a semaphore give; the driver callbacks
 *    run on the per-device worker thread with the port serializer held -
 *    the ISR never calls a driver callback;
 *  - xfer buffers are 64-byte aligned (the imported drivers exclusively
 *    use usbd_get_buffer, so the shim owns all DMA memory);
 *  - descriptors come from the real CherryUSB enumeration, not copies.
 *
 * STRUCTS AND SEAMS OF THIS PORT (what changed from the old workspace)
 *  - The device/pipe/xfer objects are the REAL NetBSD structs from
 *    usbdivar.h: ud_ddesc, ud_cdesc (a pointer into a contiguous config
 *    descriptor buffer built here from hport->config), ud_ifaces[] with
 *    ui_idesc/ui_endpoints/ue_edesc, ud_ep0/ud_ep0desc, ud_speed, ud_addr,
 *    ud_depth, ud_bus.  The shim's private state lives in thin wrappers
 *    (struct usbdi_dev/pipe/xfer) whose first member is the real struct,
 *    recovered with container_of - no upstream field is repurposed.  The
 *    pipe's real up_queue/ux_next is the parked-queue (NetBSD's up_queue
 *    semantics); up_endpoint->ue_toggle mirrors the data toggle.
 *  - Interrupt protection: <hal/ipl.h> of the old port maps to
 *    osKernelLock() and cannot guard a ring whose producer is a true ISR.
 *    This unit uses CherryUSB's own critical section instead
 *    (usb_osal_enter/leave_critical_section, i.e. ThreadX TX_DISABLE /
 *    TX_RESTORE, nested-safe around the EHCI ISR's own section).
 *  - Threads: this unit creates its own two per-device workers (sized
 *    here, not through wlan_port_thread_create()) because the urb worker
 *    runs the driver's completion path (urtwn_rxeof -> ieee80211_input)
 *    and needs the old harness's 32 KiB, while the shared
 *    CONFIG_WLAN_WORKER_STACK key stays at the netbsd line's 8 KiB.
 *  - Serialization stays wlan_port_serializer_lock/unlock around every
 *    driver entry (the splnet() discipline of the imported code).
 *  - Time: wlan_port_now_ms() has no implementation in this repo's osal,
 *    so the shim carries its own clock (wlan_usbdi_now_ms, CMSIS kernel
 *    tick).  Do not reintroduce a dependency on the unimplemented symbol.
 *
 * KNOWN DEVIATIONS (deliberate, carried over from the board-proven shim)
 *  - Armed xfers are never killed by the watchdog: a bulk OUT urb is
 *    stamped at arm time with the toggle of the packet AFTER the transfer
 *    and the completion path writes that value back for every status,
 *    CANCELLED included, so killing a partially consumed bulk OUT
 *    re-seeds the pipe with a toggle the device does not hold and the
 *    pipe is dead forever.  Only parked xfers time out.  Teardown paths
 *    (pipe close/abort, xfer destroy) still kill, because the pipe dies
 *    right after.
 *  - No synthetic USBD_SHORT_XFER: a short completion is reported as
 *    USBD_NORMAL_COMPLETION with the real ux_actlen (which is what the
 *    urtwn rx/tx completion paths act on).  All of urtwn's data pipes
 *    pass USBD_SHORT_XFER_OK anyway.
 *  - USBH_URB_ZERO_PACKET (USBD_FORCE_SHORT_XFER -> explicit ZLP for a
 *    bulk OUT that ends on a max-packet boundary) is still forwarded, but
 *    the pinned CherryUSB tree declares neither the flag nor its EHCI
 *    handling - upstream's transfer_flags field is inert here.  The value
 *    is defined locally so the intent compiles; the hunk that makes it
 *    real is the one patches/README.md keeps as a known candidate.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include "usbh_core.h"
#include "usb_osal.h"
#include "usb_dcache.h"

/* the shim needs the raw CherryUSB speed/size codes and the NetBSD ones in
 * one unit, and they disagree above FULL (dev/usb/usbdef.h vs usb.h): read
 * the cherryusb values here, then let the NetBSD definitions win below */
enum {
	USBH_CB_SPEED_UNKNOWN    = USB_SPEED_UNKNOWN,
	USBH_CB_SPEED_LOW        = USB_SPEED_LOW,
	USBH_CB_SPEED_FULL       = USB_SPEED_FULL,
	USBH_CB_SPEED_HIGH       = USB_SPEED_HIGH,
	USBH_CB_SPEED_WIRELESS   = USB_SPEED_WIRELESS,
	USBH_CB_SPEED_SUPER      = USB_SPEED_SUPER,
	USBH_CB_SPEED_SUPER_PLUS = USB_SPEED_SUPER_PLUS,
};

#undef USB_MAX_DEVICES
#undef USB_SPEED_LOW
#undef USB_SPEED_FULL
#undef USB_SPEED_HIGH
#undef USB_SPEED_WIRELESS
#undef USB_SPEED_SUPER
#undef USB_SPEED_SUPER_PLUS

/* upstream's pin carries the transfer_flags field but neither the flag
 * macros nor the EHCI ZLP handling (patches/README.md keeps that hunk as
 * a known candidate); define the value locally so the forwarding below
 * compiles, and keep the comment honest about it being inert on this pin */
#ifndef USBH_URB_ZERO_PACKET
#define USBH_URB_ZERO_PACKET (1u << 0)
#endif

#include <sys/queue.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usb_quirks.h>

#include "port.h"
#include "wlan_port_cmsis.h"
#include "wlan_cherryusb.h"

#define USBD_SHIM_ALIGN 64
#define USBD_SHIM_MAX_IFACES CONFIG_USBHOST_MAX_INTERFACES
#define USBD_SHIM_MAX_EPS CONFIG_USBHOST_MAX_ENDPOINTS
#define USBD_SHIM_CTRL_BOUNCE CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define USBD_SHIM_RING  64	/* power of two; >= 3x the 17 in-flight xfers */
#define USBD_SHIM_RING_MASK (USBD_SHIM_RING - 1)
#define USBD_SHIM_MAGIC 0xa510be55u

/* Worker stacks (bytes): the urb worker carries the driver completion path
 * (see the file header); the taskq worker runs the driver's usb tasks. */
#define USBD_SHIM_URB_STACK	32768
#define USBD_SHIM_TASKQ_STACK	16384

/* the shim copies descriptor bytes between the two worlds: the layouts
 * must agree byte for byte or the NetBSD view would describe another
 * device than the one CherryUSB enumerated */
_Static_assert(sizeof(usb_device_descriptor_t) ==
    sizeof(struct usb_device_descriptor), "device descriptor size mismatch");
_Static_assert(sizeof(usb_config_descriptor_t) ==
    sizeof(struct usb_configuration_descriptor), "config descriptor size mismatch");
_Static_assert(sizeof(usb_interface_descriptor_t) ==
    sizeof(struct usb_interface_descriptor), "interface descriptor size mismatch");
_Static_assert(sizeof(usb_endpoint_descriptor_t) ==
    sizeof(struct usb_endpoint_descriptor), "endpoint descriptor size mismatch");
_Static_assert(CONFIG_USB_ALIGN_SIZE == USBD_SHIM_ALIGN,
    "cherryusb and the shim must agree on the DMA alignment");
_Static_assert(USBD_SHIM_CTRL_BOUNCE >= 512, "control bounce too small");

/* ------------------------------------------------------------------ */

struct usbdi_dev;
struct usbdi_pipe;
struct usbdi_xfer;

SLIST_HEAD(usbdi_xfer_list, usbdi_xfer);

struct usbdi_xfer {
	struct usbd_xfer xfer;		/* the object the driver holds */
	struct usbdi_pipe *pipe;
	struct usbh_urb urb;
	void *dma_raw;	/* the block dma_buf/ux_buf points inside of; the
			 * free must go to this one - freeing the aligned
			 * interior point reads a phantom heap header and
			 * steers coalescing into live neighbors */
	unsigned submit_ms;
	unsigned arm_ms;
	unsigned done_ms;
	unsigned wd_deadline;	/* watchdog deadline (wlan_usbdi_now_ms);
				 * 0 = unarmed (USBD_NO_TIMEOUT) */
	uint8_t in_flight;	/* at the HCD, not yet dispatched to a cb */
	uint8_t on_pending;	/* linked into pipe->pending; a SLIST_REMOVE of
				 * a non-member walks off into the heap and
				 * both bends the list and rewrites random
				 * allocator words */
	uint8_t on_xq;		/* parked on the pipe's up_queue */
	uint8_t on_wd;		/* linked into dev->in_flight_xfers */
	unsigned magic;		/* USBD_SHIM_MAGIC, checked on ring dequeue */
	SLIST_ENTRY(usbdi_xfer) pending_next;
	SLIST_ENTRY(usbdi_xfer) wd_next;
};

struct usbdi_pipe {
	struct usbd_pipe pipe;		/* the object the driver holds */
	struct usbdi_dev *dev;
	usb_endpoint_descriptor_t ed;	/* the NetBSD view of the endpoint */
	struct usbd_endpoint endpoint;	/* real: ue_edesc points into cdesc */
	struct usb_endpoint_descriptor *cherry_ep; /* inside hport->config */
	uint8_t data_toggle;
	/* NetBSD up_queue: xfers parked while the pipe's slot at the HCD is
	 * taken.  The cherryusb host contract is one urb per endpoint in
	 * flight; ordering is enforced here instead.  All xq state is
	 * mutated under the port serializer (callers hold it around
	 * usbd_transfer; the worker takes it in usbdi_pipe_kick). */
	uint8_t xq_busy;	/* an armed xfer holds the pipe */
	unsigned xq_len;
	/* xfers handed to the HCD and not yet dispatched to a driver
	 * callback; usbdi_abort/close walk it to kill the armed one */
	SLIST_HEAD(, usbdi_xfer) pending;
};

struct usbdi_dev {
	struct usbd_device dev;		/* the object the driver holds */
	struct usbd_bus bus;		/* ud_bus; ub_lock is a real mutex so a
					 * future usbd_lock_pipe() cannot NULL-deref */
	kmutex_t bus_lock;
	void *cdesc_raw;	/* the contiguous config buffer ud_cdesc
				 * points into */
	uint8_t nifaces;	/* interfaces in ud_ifaces[] (and walked by
				 * the failure-path teardown) */
	void *drv_ctx;		/* caller's bookkeeping (chip driver) */
	struct usbh_hubport *hport;
	volatile int running;

	/* urb completion ring (producer: EHCI interrupt, single; consumer:
	 * the urb worker, single).  SPSC with head/tail indices and no
	 * locks: the producer can run on a true ISR while the consumer
	 * runs on any core, so the ordering contract is store-data,
	 * barrier, store-index / load-index, barrier, load-data. */
	struct usbd_xfer *ring[USBD_SHIM_RING];
	volatile unsigned ring_head;	/* consumer index */
	volatile unsigned ring_tail;	/* producer index */
	usb_osal_sem_t ring_sem;

	/* usb task queue ring */
	struct usb_task *tasks[USBD_SHIM_RING];
	volatile unsigned task_head;
	usb_osal_sem_t task_sem;

	kmutex_t wq_mtx;
	kcondvar_t wq_cv;
	void *urb_worker;
	void *taskq_worker;
	int workers_started;

	/* xfers handed to the HCD and not yet dispatched to a driver
	 * callback (submit path inc, worker dec, both under the shim's
	 * critical section) */
	unsigned in_flight_cnt;

	/* all async xfers awaiting completion, for the watchdog sweep
	 * (submit inserts, worker removes, both under the critical
	 * section) */
	struct usbdi_xfer_list in_flight_xfers;
};

/* the shim's three wrappers all have their real struct first: recover the
 * shim state from the pointer the driver holds */
static inline struct usbdi_xfer *usbdi_xf(struct usbd_xfer *xfer) {
	return (struct usbdi_xfer *)(void *)((char *)xfer -
	    offsetof(struct usbdi_xfer, xfer));
}

static inline struct usbdi_pipe *usbdi_pi(struct usbd_pipe *pipe) {
	return (struct usbdi_pipe *)(void *)((char *)pipe -
	    offsetof(struct usbdi_pipe, pipe));
}

static inline struct usbdi_dev *usbdi_dv(struct usbd_device *dev) {
	return (struct usbdi_dev *)(void *)((char *)dev -
	    offsetof(struct usbdi_dev, dev));
}

/* control bounce region: EHCI requires 64-byte aligned setup/data while
 * the driver buffers live on the stack or in the softc.  Two separate
 * objects in one 64-byte-aligned struct: the setup packet is 8 bytes, so
 * a payload placed right after it would still be misaligned. */
struct usbdi_ctrl_bounce {
	struct usb_setup_packet setup;
	uint8_t pad[USBD_SHIM_ALIGN - sizeof(struct usb_setup_packet)];
	uint8_t buf[USBD_SHIM_CTRL_BOUNCE];
};

_Static_assert(offsetof(struct usbdi_ctrl_bounce, buf) == USBD_SHIM_ALIGN,
    "control bounce payload must be 64-byte aligned");
_Static_assert(offsetof(struct usbdi_ctrl_bounce, setup) == 0,
    "control bounce setup must be 64-byte aligned");

static struct usbdi_ctrl_bounce s_ctrl
	__attribute__((aligned(USBD_SHIM_ALIGN)));
static kmutex_t s_ctrl_mtx;
static int s_shim_ready;

/* usbdi_ipl_save/restore: the old shim's ipl_save pair, mapped onto
 * CherryUSB's own critical section.  <hal/ipl.h> of this port is
 * osKernelLock (thread-grade only) and cannot guard a ring whose producer
 * is a true ISR; the CherryUSB primitive is the interrupt disable the
 * EHCI port already wraps its dispatch in, and it nests. */
static inline size_t usbdi_ipl_save(void) {
	return usb_osal_enter_critical_section();
}

static inline void usbdi_ipl_restore(size_t flags) {
	usb_osal_leave_critical_section(flags);
}

static void shim_locks_init(void) {
	if (s_shim_ready) {
		return;
	}
	s_shim_ready = 1;
	mutex_init(&s_ctrl_mtx, MUTEX_DEFAULT, IPL_VM);
}

/* Monotonic milliseconds off the CMSIS kernel tick (1 ms on this build;
 * the scaling keeps the unit honest if that ever changes).  See
 * wlan_cherryusb.h for why this is not wlan_port_now_ms(). */
unsigned int wlan_usbdi_now_ms(void) {
	uint32_t freq = osKernelGetTickFreq();

	if (freq == 0U) {
		return (unsigned int) osKernelGetTickCount();
	}
	return (unsigned int) ((uint64_t) osKernelGetTickCount() * 1000ULL /
	    (uint64_t) freq);
}

/* ue_toggle is the real struct's field for the endpoint's data toggle;
 * this shim's canonical copy is data_toggle (what the old file used), so
 * keep the two in step at every write */
static inline void usbdi_pipe_set_toggle(struct usbdi_pipe *pipe,
	uint8_t toggle) {
	pipe->data_toggle = toggle;
	pipe->endpoint.ue_toggle = (int) toggle;
}

static usbd_status usbdi_map_err(int cherry_err) {
	switch (cherry_err) {
	case 0:
		return USBD_NORMAL_COMPLETION;
	case -USB_ERR_TIMEOUT:
		return USBD_TIMEOUT;
	case -USB_ERR_SHUTDOWN:
	case -USB_ERR_NOTCONN:
		return USBD_CANCELLED;
	case -USB_ERR_STALL:
		return USBD_STALLED;
	case -USB_ERR_NOMEM:
		return USBD_NOMEM;
	default:
		return USBD_IOERROR;
	}
}

/* the live devices, for the per-device part of the stats dump (the attach
 * path appends; this harness never detaches an object) */
#define USBD_SHIM_DEV_MAX 4
static struct usbdi_dev *s_usbdi_devs[USBD_SHIM_DEV_MAX];
static unsigned s_usbdi_ndevs;

/* NO DEVICE STRINGS.  These dongles carry no readable product string, and
 * naming one after the first driver that needed this looked like a
 * different chip had attached, so nothing here reads ud_vendor/ud_product
 * strings; ud_quirks only has to be non-NULL (usbdivar.h: "always set"). */
static const struct usbd_quirks usbdi_no_quirk = {
	.uq_flags = 0,
	.desc = NULL,
};

/* attribution counters for the completion path, dumped by `wlan usbstats`;
 * each field pins one way a completion can die between the EHCI interrupt
 * and the driver callback */
static struct wlan_usb_stats {
	unsigned submit;	/* usbd_transfer calls */
	unsigned submit_fail;	/* usbh_submit_urb != 0 (synthesized cb) */
	unsigned complete;	/* urb->complete entered (EHCI IRQ ctx) */
	unsigned guard_drop;	/* complete: stale/in_flight guard early-return */
	unsigned post_ok;	/* enqueued into the completion ring */
	unsigned post_drop;	/* ring full: xfer lost, callback never runs */
	unsigned worker_run;	/* driver callbacks dispatched */
	unsigned kill_calls;	/* shim-side usbh_kill_urb invocations */
	unsigned wd_timeouts;	/* watchdog-killed in-flight xfers */
	unsigned tx_submit;	/* bulk OUT submits */
	unsigned tx_complete;	/* bulk OUT callbacks dispatched */
	unsigned rx_submit;	/* bulk IN submits */
	unsigned rx_complete;	/* bulk IN callbacks dispatched */
	unsigned in_flight_peak;/* max xfers handed to the HCD at once */
	unsigned ctrl_fail;	/* control xfers failed after retries */
	unsigned ctrl_retry;	/* control xfer re-submissions after timeout */
	unsigned stall_clear;	/* CLEAR_FEATURE(ENDPOINT_HALT) sent (bulk) */
	unsigned task_drop;	/* usb_add_task: task ring full, task lost */
	unsigned task_busy;	/* usb_add_task: already queued (normal) */
	int rx_err_last;	/* last negative RX completion (cherryusb code) */
	unsigned q_kicks;	/* xfers armed from the pipe queue */
	unsigned q_peak;	/* max xfers parked on one pipe */
	unsigned q_flush;	/* xfers returned CANCELLED by abort/close */
	unsigned q_timeout;	/* queued xfers timed out before arming */
	unsigned q_orphan;	/* destroyed while parked (must stay 0) */
} wlan_usb_stats;

static struct wlan_usb_latency {
	unsigned samples;
	unsigned queue_total_ms;
	unsigned hcd_total_ms;
	unsigned wake_total_ms;
	unsigned queue_max_ms;
	unsigned hcd_max_ms;
	unsigned wake_max_ms;
} wlan_usb_tx_latency, wlan_usb_rx_latency;

static void wlan_usb_latency_record(struct wlan_usb_latency *lat,
	const struct usbdi_xfer *w, unsigned now_ms) {
	unsigned queue_ms = w->arm_ms - w->submit_ms;
	unsigned hcd_ms = w->done_ms - w->arm_ms;
	unsigned wake_ms = now_ms - w->done_ms;

	lat->samples++;
	lat->queue_total_ms += queue_ms;
	lat->hcd_total_ms += hcd_ms;
	lat->wake_total_ms += wake_ms;
	if (queue_ms > lat->queue_max_ms)
		lat->queue_max_ms = queue_ms;
	if (hcd_ms > lat->hcd_max_ms)
		lat->hcd_max_ms = hcd_ms;
	if (wake_ms > lat->wake_max_ms)
		lat->wake_max_ms = wake_ms;
}

static int usbdi_pipe_is_tx(const struct usbdi_pipe *pipe) {
	return (pipe != NULL &&
	    (pipe->ed.bEndpointAddress & UE_DIR_IN) == 0U);
}

static void usbdi_ring_post(struct usbdi_dev *dev, struct usbd_xfer *xfer) {
	unsigned tail = dev->ring_tail;

	if ((unsigned) (tail - dev->ring_head) >= USBD_SHIM_RING) {
		/* ring full: the xfer is silently lost today (no error
		 * status ever reaches the driver); count it so the
		 * `wlan usbstats` dump makes the loss visible.  With 17
		 * in-flight xfers max and a 64-deep ring this should
		 * never fire. */
		wlan_usb_stats.post_drop++;
		usb_osal_sem_give(dev->ring_sem);
		return;
	}
	dev->ring[tail & USBD_SHIM_RING_MASK] = xfer;
	__asm__ __volatile__("dmb ish" ::: "memory");
	dev->ring_tail = tail + 1;
	wlan_usb_stats.post_ok++;
	usb_osal_sem_give(dev->ring_sem);
}

/* --- pipe queue (NetBSD up_queue) ---------------------------------- */

/* remove a parked xfer from its pipe queue; callers hold the serializer
 * and have checked on_xq (SIMPLEQ_REMOVE walks to the element and would
 * run off the end for a non-member) */
static void usbdi_xq_unlink(struct usbdi_pipe *pipe, struct usbdi_xfer *w) {
	SIMPLEQ_REMOVE(&pipe->pipe.up_queue, &w->xfer, usbd_xfer, ux_next);
	if (pipe->xq_len > 0U) {
		pipe->xq_len--;
	}
}

/* return every parked xfer to the driver with one status, without
 * touching the HCD (they never reached it).  Completions are posted to
 * the ring so the callbacks still run on the urb worker.  Callers hold
 * the serializer. */
static void usbdi_xq_flush(struct usbdi_pipe *pipe, usbd_status status) {
	struct usbd_xfer *xfer;
	struct usbdi_xfer *w;

	while ((xfer = SIMPLEQ_FIRST(&pipe->pipe.up_queue)) != NULL) {
		w = usbdi_xf(xfer);
		SIMPLEQ_REMOVE_HEAD(&pipe->pipe.up_queue, ux_next);
		w->on_xq = 0;
		if (pipe->xq_len > 0U) {
			pipe->xq_len--;
		}
		w->wd_deadline = 0U;
		xfer->ux_status = status;
		xfer->ux_actlen = 0;
		wlan_usb_stats.q_flush++;
		usbdi_ring_post(pipe->dev, xfer);
	}
}

/* ------------------------------------------------------------------ */
/* device/interface construction */

/* the NetBSD config descriptor view is built once, contiguously, from
 * cherryusb's parsed configuration: config desc, then per interface its
 * altsetting[0] descriptor followed by its endpoint descriptors - the
 * layout usbd_find_idesc/usbd_find_edesc walk in the real stack */
static int usbdi_build_ifaces(struct usbdi_dev *d) {
	struct usbh_hubport *hport = d->hport;
	struct usb_configuration_descriptor *ccd =
	    &hport->config.config_desc;
	usb_config_descriptor_t *ncd;
	usb_interface_descriptor_t *nid;
	usb_endpoint_descriptor_t *ned;
	uint8_t buf[USBD_SHIM_MAX_IFACES];
	uint8_t nifc = ccd->bNumInterfaces;
	uint16_t total;
	uint8_t *raw;
	uintptr_t off;
	uint8_t i, j;

	if (nifc == 0U || nifc > USBD_SHIM_MAX_IFACES) {
		return -1;
	}

	total = USB_CONFIG_DESCRIPTOR_SIZE;
	for (i = 0; i < nifc; i++) {
		uint8_t nep = (uint8_t)
		    hport->config.intf[i].altsetting[0].intf_desc.bNumEndpoints;

		if (nep > USBD_SHIM_MAX_EPS) {
			return -1;
		}
		buf[i] = nep;
		total = (uint16_t) (total + USB_INTERFACE_DESCRIPTOR_SIZE +
		    (uint16_t) nep * USB_ENDPOINT_DESCRIPTOR_SIZE);
	}

	raw = wlan_kmalloc(total, M_WAITOK | M_ZERO, M_USB);
	if (raw == NULL) {
		return -1;
	}
	d->cdesc_raw = raw;

	ncd = (usb_config_descriptor_t *) (void *) raw;
	memcpy(ncd, ccd, sizeof(*ncd));
	ncd->bLength = USB_CONFIG_DESCRIPTOR_SIZE;
	ncd->bDescriptorType = UDESC_CONFIG;
	USETW(ncd->wTotalLength, total);
	ncd->bNumInterface = nifc;
	d->dev.ud_cdesc = ncd;

	d->dev.ud_ifaces = wlan_kmalloc(
	    (size_t) nifc * sizeof(struct usbd_interface), M_WAITOK | M_ZERO,
	    M_USB);
	if (d->dev.ud_ifaces == NULL) {
		return -1;
	}
	d->nifaces = nifc;

	off = USB_CONFIG_DESCRIPTOR_SIZE;
	for (i = 0; i < nifc; i++) {
		struct usbh_interface_altsetting *alt =
		    &hport->config.intf[i].altsetting[0];
		struct usbd_interface *uif = &d->dev.ud_ifaces[i];

		nid = (usb_interface_descriptor_t *) (void *) (raw + off);
		memcpy(nid, &alt->intf_desc, sizeof(*nid));
		off += USB_INTERFACE_DESCRIPTOR_SIZE;

		uif->ui_dev = &d->dev;
		uif->ui_idesc = nid;
		uif->ui_index = (int) i;
		uif->ui_altindex = 0;
		uif->ui_busy = 0;
		if (buf[i] != 0U) {
			uif->ui_endpoints = wlan_kmalloc(
			    (size_t) buf[i] * sizeof(struct usbd_endpoint),
			    M_WAITOK | M_ZERO, M_USB);
			if (uif->ui_endpoints == NULL) {
				return -1;
			}
		}
		for (j = 0; j < buf[i]; j++) {
			ned = (usb_endpoint_descriptor_t *) (void *)
			    (raw + off);
			memcpy(ned, &alt->ep[j].ep_desc, sizeof(*ned));
			off += USB_ENDPOINT_DESCRIPTOR_SIZE;
			uif->ui_endpoints[j].ue_edesc = ned;
			uif->ui_endpoints[j].ue_refcnt = 0;
			uif->ui_endpoints[j].ue_toggle = 0;
		}
	}

	return (off == (uintptr_t) total) ? 0 : -1;
}

/* the cherryusb endpoint struct matching the index-th endpoint of an
 * interface (both views are built in the same order) */
static struct usb_endpoint_descriptor *usbdi_cherry_ep(
	struct usbd_interface *iface, uint8_t index) {
	struct usbdi_dev *d = usbdi_dv(iface->ui_dev);

	return &d->hport->config.intf[iface->ui_index]
	    .altsetting[0].ep[index].ep_desc;
}

/* free what a failed usbdi_build_ifaces()/register left behind; only the
 * attach failure path calls this (a live device is never freed) */
static void usbdi_dev_free(struct usbdi_dev *d) {
	uint8_t i;

	if (d->dev.ud_ifaces != NULL) {
		/* only the interfaces this build actually filled: the array
		 * is nifaces long (<= CONFIG_USBHOST_MAX_INTERFACES), not
		 * the constant */
		for (i = 0; i < d->nifaces; i++) {
			if (d->dev.ud_ifaces[i].ui_endpoints != NULL) {
				wlan_kfree(d->dev.ud_ifaces[i].ui_endpoints,
				    M_USB);
			}
		}
		wlan_kfree(d->dev.ud_ifaces, M_USB);
	}
	if (d->cdesc_raw != NULL) {
		wlan_kfree(d->cdesc_raw, M_USB);
	}
	wlan_kfree(d, M_USB);
}

static struct usbdi_dev *usbdi_dev_register(struct usbh_hubport *hport,
	void *drv_ctx) {
	struct usbdi_dev *d;
	struct usb_device_descriptor *cdd = &hport->device_desc;
	uint16_t mps;

	d = wlan_kmalloc(sizeof(*d), M_WAITOK | M_ZERO, M_USB);
	if (d == NULL) {
		return NULL;
	}
	d->hport = hport;
	d->drv_ctx = drv_ctx;
	SLIST_INIT(&d->in_flight_xfers);

	memcpy(&d->dev.ud_ddesc, cdd, sizeof(d->dev.ud_ddesc));

	/* bus shell: the real ud_bus points here, and ub_lock is a live
	 * mutex so a future usbd_lock_pipe() cannot dereference NULL */
	mutex_init(&d->bus_lock, MUTEX_DEFAULT, IPL_VM);
	d->bus.ub_hcpriv = hport;
	d->bus.ub_revision = USBREV_2_0;
	d->bus.ub_hctype = USBHCTYPE_EHCI;
	d->bus.ub_busnum = (hport->bus != NULL) ? hport->bus->busid : 0;
	d->bus.ub_pipesize = (uint32_t) sizeof(struct usbdi_pipe);
	d->bus.ub_usedma = true;
	d->bus.ub_dmatag = NULL;
	d->bus.ub_lock = &d->bus_lock;
	d->dev.ud_bus = &d->bus;

	d->dev.ud_addr = hport->dev_addr;
	d->dev.ud_depth = hport->depth;
	d->dev.ud_speed = 0;
	switch (hport->speed) {
	case USBH_CB_SPEED_LOW:
		d->dev.ud_speed = USB_SPEED_LOW;
		break;
	case USBH_CB_SPEED_FULL:
		d->dev.ud_speed = USB_SPEED_FULL;
		break;
	case USBH_CB_SPEED_HIGH:
		d->dev.ud_speed = USB_SPEED_HIGH;
		break;
	case USBH_CB_SPEED_SUPER:
	case USBH_CB_SPEED_SUPER_PLUS:
		/* no xHCI in a cherryusb image today; keep the mapping honest
		 * rather than silently claiming high speed */
		d->dev.ud_speed = USB_SPEED_SUPER;
		break;
	default:
		d->dev.ud_speed = USB_SPEED_FULL;
		break;
	}
	d->dev.ud_power = (uint16_t) ((uint16_t)
	    hport->config.config_desc.bMaxPower * UC_POWER_FACTOR);
	d->dev.ud_selfpowered =
	    (hport->config.config_desc.bmAttributes & UC_SELF_POWERED) != 0U;
	d->dev.ud_langid = USBD_NOLANG;
	d->dev.ud_config = hport->config.config_desc.bConfigurationValue;
	d->dev.ud_configidx = 0;
	d->dev.ud_cookie.cookie = 0U;
	d->dev.ud_quirks = &usbdi_no_quirk;

	/* pipe 0's endpoint descriptor (usbdivar.h's usb_subr.c does the
	 * same before any pipe exists) */
	mps = hport->ep0.wMaxPacketSize;
	if (mps == 0U) {
		mps = cdd->bMaxPacketSize0;
	}
	d->dev.ud_ep0desc.bLength = USB_ENDPOINT_DESCRIPTOR_SIZE;
	d->dev.ud_ep0desc.bDescriptorType = UDESC_ENDPOINT;
	d->dev.ud_ep0desc.bEndpointAddress = USB_CONTROL_ENDPOINT;
	d->dev.ud_ep0desc.bmAttributes = UE_CONTROL;
	USETW(d->dev.ud_ep0desc.wMaxPacketSize, mps);
	d->dev.ud_ep0desc.bInterval = 0;
	d->dev.ud_ep0.ue_edesc = &d->dev.ud_ep0desc;
	d->dev.ud_ep0.ue_refcnt = 0;
	d->dev.ud_ep0.ue_toggle = 0;
	/* no ud_pipe0: the shim's control path never goes through a pipe
	 * object (it is usbh_control_transfer), so nothing may dereference
	 * it without a NULL check */
	d->dev.ud_pipe0 = NULL;

	if (usbdi_build_ifaces(d) != 0) {
		usbdi_dev_free(d);
		return NULL;
	}
	return d;
}

/* ------------------------------------------------------------------ */

usb_device_descriptor_t *usbd_get_device_descriptor(struct usbd_device *dev) {
	return (dev != NULL) ? &dev->ud_ddesc : NULL;
}

usb_config_descriptor_t *usbd_get_config_descriptor(struct usbd_device *dev) {
	return (dev != NULL) ? dev->ud_cdesc : NULL;
}

uint8_t usbd_get_speed(struct usbd_device *dev) {
	return (dev != NULL) ? dev->ud_speed : 0U;
}

usb_interface_descriptor_t *usbd_get_interface_descriptor(
	struct usbd_interface *iface) {
	return (iface != NULL) ? iface->ui_idesc : NULL;
}

usb_endpoint_descriptor_t *usbd_interface2endpoint_descriptor(
	struct usbd_interface *iface, uint8_t index) {
	if (iface == NULL || iface->ui_idesc == NULL ||
	    iface->ui_endpoints == NULL) {
		return NULL;
	}
	if (index >= iface->ui_idesc->bNumEndpoints) {
		return NULL;
	}
	return iface->ui_endpoints[index].ue_edesc;
}

void usbd_interface2device_handle(struct usbd_interface *iface,
	struct usbd_device **dev) {
	if (iface != NULL && dev != NULL) {
		*dev = iface->ui_dev;
	}
}

usbd_status usbd_device2interface_handle(struct usbd_device *dev,
	uint8_t ifindex, struct usbd_interface **iface) {
	if (dev == NULL || iface == NULL) {
		return USBD_INVAL;
	}
	if (dev->ud_cdesc == NULL) {
		return USBD_NOT_CONFIGURED;
	}
	if (ifindex >= dev->ud_cdesc->bNumInterface) {
		return USBD_INVAL;
	}
	*iface = &dev->ud_ifaces[ifindex];
	return USBD_NORMAL_COMPLETION;
}

/* ------------------------------------------------------------------ */

usbd_status usbd_open_pipe(struct usbd_interface *iface, uint8_t address,
	uint8_t flags, struct usbd_pipe **pipep) {
	struct usbdi_pipe *pipe;
	usb_interface_descriptor_t *id;
	usb_endpoint_descriptor_t *ed;
	uint8_t i;

	/* per-URB QH: exclusivity is inherent; the flag is still recorded in
	 * the real up_flags field for anything that reads it back */
	if (iface == NULL || iface->ui_dev == NULL || pipep == NULL) {
		return USBD_INVAL;
	}

	pipe = wlan_kmalloc(sizeof(*pipe), M_WAITOK | M_ZERO, M_USB);
	if (pipe == NULL) {
		return USBD_NOMEM;
	}
	pipe->dev = usbdi_dv(iface->ui_dev);
	pipe->pipe.up_dev = iface->ui_dev;
	pipe->pipe.up_iface = iface;
	pipe->pipe.up_flags = flags;
	pipe->pipe.up_running = 0;
	pipe->pipe.up_serialise = false; /* the shim owns the FIFO */
	SIMPLEQ_INIT(&pipe->pipe.up_queue);
	SLIST_INIT(&pipe->pending);

	id = usbd_get_interface_descriptor(iface);
	ed = NULL;
	if (id != NULL) {
		for (i = 0; i < id->bNumEndpoints; i++) {
			ed = usbd_interface2endpoint_descriptor(iface, i);
			if (ed != NULL && ed->bEndpointAddress == address) {
				break;
			}
			ed = NULL;
		}
	}
	if (ed == NULL) {
		wlan_kfree(pipe, M_USB);
		return USBD_INVAL;
	}
	pipe->ed = *ed;
	pipe->endpoint.ue_edesc = ed;
	pipe->endpoint.ue_refcnt = 0;
	pipe->endpoint.ue_toggle = 0;
	pipe->pipe.up_endpoint = &pipe->endpoint;
	pipe->cherry_ep = usbdi_cherry_ep(iface, i);
	usbdi_pipe_set_toggle(pipe, 0);
	*pipep = &pipe->pipe;
	return USBD_NORMAL_COMPLETION;
}

void usbd_close_pipe(struct usbd_pipe *pipe) {
	struct usbdi_pipe *p;
	struct usbdi_xfer *w;
	struct usbdi_xfer *next;
	size_t flags;

	if (pipe == NULL) {
		return;
	}
	p = usbdi_pi(pipe);
	wlan_port_serializer_lock();
	usbdi_xq_flush(p, USBD_CANCELLED);
	wlan_port_serializer_unlock();
	flags = usbdi_ipl_save();
	SLIST_FOREACH_SAFE(w, &p->pending, pending_next, next) {
		if (w->in_flight) {
			wlan_usb_stats.kill_calls++;
			(void) usbh_kill_urb(&w->urb);
		}
	}
	usbdi_ipl_restore(flags);
	pipe->up_running = 0;
	wlan_kfree(p, M_USB);
}

void usbd_abort_pipe(struct usbd_pipe *pipe) {
	struct usbdi_pipe *p;
	struct usbdi_xfer *w;
	size_t flags;

	if (pipe == NULL) {
		return;
	}
	p = usbdi_pi(pipe);
	/* NetBSD aborts the whole pipe: the queued xfers never reached the
	 * HCD, so they come straight back CANCELLED; the armed one is
	 * killed below and its completion kicks the (now empty) queue */
	wlan_port_serializer_lock();
	usbdi_xq_flush(p, USBD_CANCELLED);
	wlan_port_serializer_unlock();
	flags = usbdi_ipl_save();
	SLIST_FOREACH(w, &p->pending, pending_next) {
		if (w->in_flight) {
			/* thread context only (watchdog / stop paths) */
			wlan_usb_stats.kill_calls++;
			(void) usbh_kill_urb(&w->urb);
		}
	}
	usbdi_ipl_restore(flags);
}

static usbd_status usbdi_pipe_clear_halt(struct usbdi_pipe *pipe);
void usbd_clear_endpoint_stall_async(struct usbd_pipe *pipe) {
	if (pipe == NULL) {
		return;
	}
	(void) usbdi_pipe_clear_halt(usbdi_pi(pipe));
}

/* ------------------------------------------------------------------ */

int usbd_create_xfer(struct usbd_pipe *pipe, size_t size, unsigned int flags,
	unsigned int nframes, struct usbd_xfer **xp) {
	struct usbdi_xfer *w;

	if (pipe == NULL || xp == NULL) {
		return USBD_INVAL;
	}
	w = wlan_kmalloc(sizeof(*w), M_WAITOK | M_ZERO, M_USB);
	if (w == NULL) {
		return USBD_NOMEM;
	}
	w->pipe = usbdi_pi(pipe);
	w->magic = USBD_SHIM_MAGIC;
	w->xfer.ux_pipe = pipe;
	w->xfer.ux_bus = pipe->up_dev->ud_bus;
	w->xfer.ux_flags = (uint16_t) flags;
	w->xfer.ux_nframes = (int) nframes;
	if (size != 0) {
		/* the OSAL's allocator hands back a raw block and this shim
		 * keeps the raw pointer for the free: freeing the aligned
		 * interior point would read a phantom heap header.  M_ZERO
		 * covers the window the driver sees. */
		w->dma_raw = wlan_kmalloc(size + USBD_SHIM_ALIGN,
		    M_WAITOK | M_ZERO, M_USB);
		if (w->dma_raw == NULL) {
			wlan_kfree(w, M_USB);
			return USBD_NOMEM;
		}
		w->xfer.ux_buf = (void *)(((uintptr_t) w->dma_raw +
		    USBD_SHIM_ALIGN - 1) & ~(uintptr_t) (USBD_SHIM_ALIGN - 1));
		w->xfer.ux_bufsize = (uint32_t) size;
	}
	*xp = &w->xfer;
	return USBD_NORMAL_COMPLETION;
}

void usbd_destroy_xfer(struct usbd_xfer *xfer) {
	struct usbdi_xfer *w;
	size_t flags;

	if (xfer == NULL) {
		return;
	}
	w = usbdi_xf(xfer);
	if (w->on_xq && w->pipe != NULL) {
		/* a parked xfer must have been flushed or completed before
		 * the driver destroys it; unlink defensively and stay loud */
		struct usbdi_dev *d = w->pipe->dev;

		wlan_usb_stats.q_orphan++;
		wlan_port_serializer_lock();
		flags = usbdi_ipl_save();
		usbdi_xq_unlink(w->pipe, w);
		if (w->on_wd) {
			SLIST_REMOVE(&d->in_flight_xfers, w, usbdi_xfer,
			    wd_next);
			w->on_wd = 0;
			if (d->in_flight_cnt > 0U) {
				d->in_flight_cnt--;
			}
		}
		usbdi_ipl_restore(flags);
		wlan_port_serializer_unlock();
		w->on_xq = 0;
	}
	if (w->in_flight) {
		wlan_usb_stats.kill_calls++;
		(void) usbh_kill_urb(&w->urb);
	}
	if (w->dma_raw != NULL) {
		wlan_kfree(w->dma_raw, M_USB);
	}
	wlan_kfree(w, M_USB);
}

void *usbd_get_buffer(struct usbd_xfer *xfer) {
	return (xfer != NULL) ? xfer->ux_buf : NULL;
}

void usbd_setup_xfer(struct usbd_xfer *xfer, void *priv, void *buffer,
	uint32_t length, uint16_t flags, uint32_t timeout, usbd_callback cb) {
	if (xfer == NULL) {
		return;
	}
	xfer->ux_priv = priv;
	xfer->ux_buffer = (buffer != NULL) ? buffer : xfer->ux_buf;
	xfer->ux_length = length;
	xfer->ux_actlen = 0;
	xfer->ux_flags = flags;
	xfer->ux_timeout = timeout;
	xfer->ux_callback = cb;
	xfer->ux_status = USBD_NOT_STARTED;
	xfer->ux_rqflags &= (uint8_t) ~URQ_REQUEST;
	xfer->ux_nframes = 0;
}

void usbd_get_xfer_status(struct usbd_xfer *xfer, void **priv, void **buffer,
	uint32_t *actlen, usbd_status *status) {
	if (xfer == NULL) {
		return;
	}
	if (priv != NULL) {
		*priv = xfer->ux_priv;
	}
	if (buffer != NULL) {
		*buffer = xfer->ux_buffer;
	}
	if (actlen != NULL) {
		*actlen = xfer->ux_actlen;
	}
	if (status != NULL) {
		*status = xfer->ux_status;
	}
}

/* RX buffers cross the non-coherent USB DMA boundary.  Invalidate before
 * consuming a completion, and arm with a known pattern so a short/no-write
 * completion remains distinguishable in the retained capture path. */
void usbd_rx_buffer_invalidate(void *buffer, uint32_t length) {
	if (buffer != NULL && length != 0U) {
		usb_dcache_invalidate((uintptr_t) buffer, length);
	}
}

void usbd_rx_buffer_arm(void *buffer, uint32_t length) {
	if (buffer != NULL && length != 0U) {
		memset(buffer, 0xa5, length);
		usb_dcache_flush((uintptr_t) buffer, length);
	}
}

/* ------------------------------------------------------------------ */
/* async transfer plumbing */

/* Trace verbosity, off by default. 1 logs async events (urb/taskq),
 * 2 additionally logs every control transfer. Errors always print. */
static unsigned wlan_trace_lvl;
/* budgeted async (task/urb) event prints, reset by wlan_usbdi_trace_reset() */
static unsigned wlan_async_trace_seq;

/* EHCI interrupt context: record the result and hand the xfer to the
 * worker; never touch the driver callback here */
static void usbdi_urb_complete(void *arg, int nbytes_or_err) {
	struct usbdi_xfer *w = arg;
	struct usbd_xfer *xfer;
	struct usbdi_dev *dev;

	wlan_usb_stats.complete++;

	/* forensics 2026-09-24: name the raw cherryusb error for RX errors */
	if (w != NULL && w->pipe != NULL &&
	    !usbdi_pipe_is_tx(w->pipe) && nbytes_or_err < 0) {
		static unsigned rx_err_print;

		wlan_usb_stats.rx_err_last = nbytes_or_err;
		if (rx_err_print < 8 || (rx_err_print & 0x7f) == 0) {
			printf("shim: rx complete err=%d (%u)\n",
			    nbytes_or_err, ++rx_err_print);
		}
	}

	/* The async completion and the worker (which re-submits the xfer)
	 * race on the same xfer.  Only accept the completion while the
	 * transfer is still in flight; once the worker took it off the
	 * pending list and re-armed it, a stale completion must not touch
	 * it again. */
	if (w == NULL || !w->in_flight || w->pipe == NULL) {
		wlan_usb_stats.guard_drop++;
		return;
	}
	xfer = &w->xfer;
	dev = w->pipe->dev;
	w->done_ms = wlan_usbdi_now_ms();
	usbdi_pipe_set_toggle(w->pipe, w->urb.data_toggle);

	if (nbytes_or_err < 0) {
		xfer->ux_status = usbdi_map_err(nbytes_or_err);
		xfer->ux_actlen = 0;
	} else {
		xfer->ux_status = USBD_NORMAL_COMPLETION;
		xfer->ux_actlen = (uint32_t) nbytes_or_err;
	}

	if (dev != NULL) {
		usbdi_ring_post(dev, xfer);
	}
}

/* fill the urb from pipe state and hand the transfer to the HCD; the
 * xfer is already on the device watchdog list and carries an armed
 * deadline from its usbd_transfer call.  On submit failure the
 * completion is synthesized onto the ring (the driver still gets its
 * callback) and the cherry error code is returned so the caller can
 * release the pipe for the next queued xfer. */
static int usbdi_pipe_arm(struct usbdi_xfer *w) {
	struct usbdi_pipe *pipe = w->pipe;
	struct usbdi_dev *d = pipe->dev;
	struct usbd_xfer *xfer = &w->xfer;
	uint8_t *buf;
	size_t flags;
	int ret;

	memset(&w->urb, 0, sizeof(w->urb));
	/* seed from the pipe: with one urb in flight per pipe this read
	 * always happens after the previous completion wrote the toggle
	 * back (usbdi_urb_complete), or after clear-halt reset it */
	w->urb.data_toggle = pipe->data_toggle;
	buf = (xfer->ux_buffer != NULL) ?
	    (uint8_t *) xfer->ux_buffer : (uint8_t *) xfer->ux_buf;
	usbh_bulk_urb_fill(&w->urb, d->hport, pipe->cherry_ep, buf,
	    xfer->ux_length,
	    0 /* timeout=0: asynchronous */, usbdi_urb_complete, w);
	{
		/* USBD_FORCE_SHORT_XFER: bulk OUT of an exact multiple of
		 * the max packet size terminates with a zero-length
		 * packet (the HCD appends it from this flag); without it
		 * the device-side bulk FIFO can hold the last full packet
		 * waiting for a short-packet delimiter.
		 *
		 * On this cherryusb pin the flag has no EHCI handling (see
		 * the file header): the field is set, the controller is
		 * not yet told. */
		uint16_t mps = (uint16_t) UE_GET_SIZE(
		    UGETW(pipe->ed.wMaxPacketSize));

		if ((xfer->ux_flags & USBD_FORCE_SHORT_XFER) != 0U &&
		    xfer->ux_length != 0U && mps != 0U &&
		    (xfer->ux_length % mps) == 0U) {
			w->urb.transfer_flags = USBH_URB_ZERO_PACKET;
		}
	}

	w->in_flight = 1;
	w->arm_ms = wlan_usbdi_now_ms();
	if (usbdi_pipe_is_tx(pipe)) {
		wlan_usb_stats.tx_submit++;
	} else {
		wlan_usb_stats.rx_submit++;
	}
	flags = usbdi_ipl_save();
	SLIST_INSERT_HEAD(&pipe->pending, w, pending_next);
	w->on_pending = 1;
	d->in_flight_cnt++;
	if (d->in_flight_cnt > wlan_usb_stats.in_flight_peak) {
		wlan_usb_stats.in_flight_peak = d->in_flight_cnt;
	}
	pipe->pipe.up_running = 1;
	usbdi_ipl_restore(flags);
	{
		static unsigned urb_subs;

		if (wlan_trace_lvl >= 1 && urb_subs < 48) {
			printf("[wlan] urb submit #%u: ep=%02x len=%u\n",
			    urb_subs, pipe->ed.bEndpointAddress,
			    xfer->ux_length);
		}
		urb_subs++;
	}

	if (d->hport == NULL) {
		ret = -USB_ERR_NOTCONN;
	} else {
		ret = usbh_submit_urb(&w->urb);
	}
	if (ret != 0) {
		/* not connected / busy: synthesize the callback so the
		 * driver can reclaim its tx_data.  Leave the xfer on
		 * in_flight_xfers: the worker's common path unlinks it
		 * from both pipe->pending and in_flight_xfers, and a
		 * second SLIST_REMOVE here would walk the list with an
		 * element that is no longer on it (undefined in
		 * NetBSD's SLIST_REMOVE).  wd_deadline is already 0, so
		 * the watchdog sweep skips it while it waits. */
		wlan_usb_stats.submit_fail++;
		w->in_flight = 0;
		w->wd_deadline = 0U;
		xfer->ux_status = usbdi_map_err(ret);
		w->done_ms = wlan_usbdi_now_ms();
		usbdi_ring_post(d, xfer);
	}
	return ret;
}

/* arm the next parked xfer, in submission order; called from the urb
 * worker when an armed transfer retires (the shape of NetBSD's HCD
 * softint restarting the pipe).  Holds the serializer so thread-side
 * usbd_transfer callers queueing behind stay exclusive.  The loop only
 * continues past a failed arm: a successful one owns the pipe again. */
static void usbdi_pipe_kick(struct usbdi_pipe *pipe) {
	struct usbd_xfer *xfer;
	struct usbdi_xfer *w;
	size_t flags;
	int ret;

	wlan_port_serializer_lock();
	for (;;) {
		flags = usbdi_ipl_save();
		xfer = SIMPLEQ_FIRST(&pipe->pipe.up_queue);
		if (xfer == NULL) {
			pipe->xq_busy = 0;
			pipe->pipe.up_running = 0;
			usbdi_ipl_restore(flags);
			break;
		}
		SIMPLEQ_REMOVE_HEAD(&pipe->pipe.up_queue, ux_next);
		w = usbdi_xf(xfer);
		w->on_xq = 0;
		if (pipe->xq_len > 0U) {
			pipe->xq_len--;
		}
		usbdi_ipl_restore(flags);
		wlan_usb_stats.q_kicks++;
		ret = usbdi_pipe_arm(w);
		if (ret == 0) {
			break;
		}
	}
	wlan_port_serializer_unlock();
}

/* worker context: unlink from the pipe, then run the driver callback */
static void usbdi_urb_work(struct usbdi_xfer *w) {
	struct usbd_xfer *xfer = &w->xfer;
	usbd_callback cb;
	void *priv;
	usbd_status status;
	size_t flags;
	int was_armed;

	if (wlan_trace_lvl >= 1 && wlan_async_trace_seq < 48) {
		printf("[wlan] urb done: status=%d actlen=%u\n",
		    (int) xfer->ux_status, xfer->ux_actlen);
	}

	was_armed = w->in_flight;
	if (was_armed && xfer->ux_status == USBD_NORMAL_COMPLETION) {
		wlan_usb_latency_record(usbdi_pipe_is_tx(w->pipe) ?
		    &wlan_usb_tx_latency : &wlan_usb_rx_latency, w,
		    wlan_usbdi_now_ms());
	}
	w->in_flight = 0;
	w->wd_deadline = 0U;
	if (w->pipe != NULL) {
		struct usbdi_dev *d = w->pipe->dev;

		flags = usbdi_ipl_save();
		/* only a member may be SLIST_REMOVEd: parked timeouts and
		 * xq flushes complete xfers that were never armed, and a
		 * blind remove here walks off into the heap bending the
		 * list and rewriting allocator words */
		if (w->on_pending) {
			SLIST_REMOVE(&w->pipe->pending, w, usbdi_xfer,
			    pending_next);
			w->on_pending = 0;
		}
		if (w->on_wd) {
			SLIST_REMOVE(&d->in_flight_xfers, w, usbdi_xfer,
			    wd_next);
			w->on_wd = 0;
			if (d->in_flight_cnt > 0U) {
				d->in_flight_cnt--;
			}
		}
		usbdi_ipl_restore(flags);
	}
	wlan_usb_stats.worker_run++;
	if (usbdi_pipe_is_tx(w->pipe)) {
		wlan_usb_stats.tx_complete++;
	} else {
		wlan_usb_stats.rx_complete++;
	}

	cb = xfer->ux_callback;
	priv = xfer->ux_priv;
	status = xfer->ux_status;
	if (status == USBD_STALLED && w->pipe != NULL &&
	    UE_GET_XFERTYPE(w->pipe->ed.bmAttributes) == UE_BULK) {
		/* the HCD never recovers a halted bulk endpoint on its
		 * own; clear the halt so the driver's re-arm actually
		 * reaches the device instead of completing STALLED
		 * forever.  This doubles as the queue's stall gate: the
		 * parked xfers arm only after the endpoint is walking
		 * again and the pipe toggle is back to DATA0. */
		(void) usbdi_pipe_clear_halt(w->pipe);
	}
	if (was_armed && w->pipe != NULL) {
		/* only an armed completion frees the pipe's slot at the
		 * HCD (a parked xfer returning by timeout/flush does not);
		 * start the next queued xfer before the driver callback
		 * so the wire keeps flowing while it runs */
		usbdi_pipe_kick(w->pipe);
	}
	if (cb != NULL) {
		/* Run the driver callback under the port serializer, the
		 * port's replacement for the splnet() discipline of the
		 * imported NetBSD code: the driver completion path
		 * (urtwn_txeof -> urtwn_start) mutates the same tx queue
		 * and free list as the transmit path, and two contexts
		 * can be inside both at once unless they exclude here.
		 * The serializer is reentrant and tsleep drops it around
		 * waits, so nested driver entry stays correct. */
		wlan_port_serializer_lock();
		cb(xfer, priv, status);
		wlan_port_serializer_unlock();
	}
}

usbd_status usbd_transfer(struct usbd_xfer *xfer) {
	struct usbdi_xfer *w;
	struct usbdi_pipe *pipe;
	struct usbdi_dev *d;
	size_t flags;

	if (xfer == NULL || xfer->ux_pipe == NULL) {
		return USBD_INVAL;
	}
	w = usbdi_xf(xfer);
	pipe = w->pipe;
	d = pipe->dev;
	if (d->hport == NULL) {
		return USBD_IOERROR;
	}
	shim_locks_init();

	wlan_usb_stats.submit++;
	w->submit_ms = wlan_usbdi_now_ms();

	/* NetBSD usbdi serves every pipe strictly FIFO (up_queue) while
	 * the cherryusb HCD contract is one urb per endpoint in flight:
	 * the EHCI port arms a fresh QH at the async ring head per urb,
	 * so several armed urbs would be served newest-first and the
	 * device emits bulk frames in arrival order.  While the pipe's
	 * slot at the HCD is taken, park here and let the completion
	 * worker arm us in submission order (usbdi_pipe_kick).  Callers
	 * hold the port serializer (the splnet() discipline), which makes
	 * this exclusive against the worker's kick. */
	flags = usbdi_ipl_save();
	SLIST_INSERT_HEAD(&d->in_flight_xfers, w, wd_next);
	w->on_wd = 1;
	xfer->ux_status = USBD_IN_PROGRESS;
	xfer->ux_actlen = 0;
	w->wd_deadline = (xfer->ux_timeout != 0U)
	    ? wlan_usbdi_now_ms() + xfer->ux_timeout : 0U;
	if (pipe->xq_busy) {
		SIMPLEQ_INSERT_TAIL(&pipe->pipe.up_queue, xfer, ux_next);
		pipe->xq_len++;
		if (pipe->xq_len > wlan_usb_stats.q_peak) {
			wlan_usb_stats.q_peak = pipe->xq_len;
		}
		w->on_xq = 1;
		usbdi_ipl_restore(flags);
		return USBD_IN_PROGRESS;
	}
	pipe->xq_busy = 1;
	usbdi_ipl_restore(flags);

	if (usbdi_pipe_arm(w) != 0) {
		/* submit failed: the synthesized completion is already on
		 * its way through the ring and nothing is armed on this
		 * pipe, so the slot is free again right now */
		flags = usbdi_ipl_save();
		pipe->xq_busy = 0;
		pipe->pipe.up_running = 0;
		usbdi_ipl_restore(flags);
	}
	return USBD_IN_PROGRESS;
}

/* ------------------------------------------------------------------ */
/* control transfers */

/* bulk STALL recovery: CLEAR_FEATURE(ENDPOINT_HALT) returns both sides
 * to DATA0.  Nothing in cherryusb ever sent one (its clear-feature paths
 * only cover hub port features), so a halted bulk endpoint used to stay
 * dead forever; the completion worker and the driver's
 * usbd_clear_endpoint_stall_async both come through here. */
static usbd_status usbdi_pipe_clear_halt(struct usbdi_pipe *pipe) {
	usb_device_request_t req;
	usbd_status err;

	if (pipe == NULL || pipe->dev == NULL || pipe->dev->hport == NULL) {
		return USBD_INVAL;
	}
	req.bmRequestType = UT_WRITE_ENDPOINT;
	req.bRequest = UR_CLEAR_FEATURE;
	USETW(req.wValue, UF_ENDPOINT_HALT);
	USETW(req.wIndex, pipe->ed.bEndpointAddress);
	USETW(req.wLength, 0);
	err = usbd_do_request(&pipe->dev->dev, &req, NULL);
	if (err == USBD_NORMAL_COMPLETION) {
		/* clear-halt resets the endpoint toggle to DATA0 on both
		 * sides; the EHCI port takes its next start toggle from
		 * the pipe (bulk OUT) or resyncs from the wire (bulk IN) */
		usbdi_pipe_set_toggle(pipe, 0);
		wlan_usb_stats.stall_clear++;
	}
	return err;
}

/* control-transfer trace counter, reset by wlan_usbdi_trace_reset() */
static unsigned wlan_ctrl_trace_seq;

static usbd_status usbdi_ctrl_xfer(struct usbd_device *dev,
	usb_device_request_t *req, void *data, int *actlen) {
	struct usbdi_dev *d;
	uint16_t len;
	int is_read;
	int ret;
	unsigned attempt;

	if (dev == NULL || req == NULL) {
		return USBD_INVAL;
	}
	d = usbdi_dv(dev);
	if (d->hport == NULL) {
		return USBD_IOERROR;
	}
	shim_locks_init();
	mutex_enter(&s_ctrl_mtx);

	len = (uint16_t) UGETW(req->wLength);
	if (len > USBD_SHIM_CTRL_BOUNCE) {
		/* the bounce is the DMA object; a longer request would need a
		 * second staging buffer */
		printf("[wlan] shim: control len %u > bounce %u\n", len,
		    (unsigned) USBD_SHIM_CTRL_BOUNCE);
		mutex_exit(&s_ctrl_mtx);
		return USBD_INVAL;
	}

	if (wlan_trace_lvl >= 2 && wlan_ctrl_trace_seq < 800) {
		printf("[wlan] ctrl #%u: type=%02x req=%02x val=%04x len=%u\n",
		    wlan_ctrl_trace_seq, req->bmRequestType, req->bRequest,
		    UGETW(req->wValue), UGETW(req->wLength));
	}
	wlan_ctrl_trace_seq++;

	s_ctrl.setup.bmRequestType = req->bmRequestType;
	s_ctrl.setup.bRequest = req->bRequest;
	s_ctrl.setup.wValue = (uint16_t) UGETW(req->wValue);
	s_ctrl.setup.wIndex = (uint16_t) UGETW(req->wIndex);
	s_ctrl.setup.wLength = len;

	is_read = (req->bmRequestType & UT_READ) != 0U;
	if (len > 0 && !is_read && data != NULL) {
		memcpy(s_ctrl.buf, data, len);
	}

	/* The device occasionally leaves ep0 unanswered for longer than
	 * usbh's fixed 500 ms control timeout (observed on LED/calib reads
	 * during scanning).  CherryUSB kills the timed-out URB, so a fresh
	 * submission is safe; give it a few chances before failing. */
	for (attempt = 0;; attempt++) {
		ret = usbh_control_transfer(d->hport, &s_ctrl.setup,
		    s_ctrl.buf);
		if (ret != -USB_ERR_TIMEOUT || attempt == 2) {
			break;
		}
		wlan_usb_stats.ctrl_retry++;
		usb_osal_msleep(10);
	}
	/* >= 0: actual length; < 0: cherryusb error code */
	if (ret < 0) {
		wlan_usb_stats.ctrl_fail++;
		printf("[wlan] ctrl xfer failed: type=%02x req=%02x val=%04x "
		    "len=%u raw=%d\n",
		    req->bmRequestType, req->bRequest, UGETW(req->wValue),
		    UGETW(req->wLength), ret);
		mutex_exit(&s_ctrl_mtx);
		return usbdi_map_err(ret);
	}
	if (wlan_trace_lvl >= 2 && wlan_ctrl_trace_seq < 802) {
		printf("[wlan] ctrl #%u done: %d\n", wlan_ctrl_trace_seq - 1,
		    ret);
	}
	if (len > 0 && is_read && data != NULL) {
		uint16_t n = (len < (uint16_t) ret) ? len : (uint16_t) ret;

		memcpy(data, s_ctrl.buf, n);
	}
	if (actlen != NULL) {
		*actlen = ret;
	}

	mutex_exit(&s_ctrl_mtx);
	return USBD_NORMAL_COMPLETION;
}

usbd_status usbd_do_request(struct usbd_device *dev,
	usb_device_request_t *req, void *data) {
	return usbdi_ctrl_xfer(dev, req, data, NULL);
}

usbd_status usbd_do_request_flags(struct usbd_device *dev,
	usb_device_request_t *req, void *data, uint16_t flags, int *actlen,
	uint32_t timeout) {
	(void) flags;
	(void) timeout; /* usbh_control_transfer: fixed 500 ms inside */
	return usbdi_ctrl_xfer(dev, req, data, actlen);
}

usbd_status usbd_set_config_no(struct usbd_device *dev, int config,
	int flags) {
	usb_device_request_t req;
	uint8_t val = 0;
	usbd_status err;

	(void) flags;
	if (dev == NULL || dev->ud_cdesc == NULL) {
		return USBD_NOT_CONFIGURED;
	}
	/* cherryusb enumeration already issued SET_CONFIGURATION(1); read
	 * it back through the aligned bounce and only complain on drift */
	memset(&req, 0, sizeof(req));
	req.bmRequestType = UT_READ_DEVICE;
	req.bRequest = UR_GET_CONFIG;
	USETW(req.wValue, 0);
	USETW(req.wIndex, 0);
	USETW(req.wLength, 1);
	err = usbd_do_request(dev, &req, &val);
	if (err != USBD_NORMAL_COMPLETION) {
		return err;
	}
	if (config != 0 && val != config) {
		printf("[wlan] shim: cfg %d != %d (enumerated), ignored\n",
		    val, config);
	}
	return USBD_NORMAL_COMPLETION;
}

/* ------------------------------------------------------------------ */
/* misc glue */

/* Note: usbdivar.h declares this as returning void, not usbd_status - the
 * old harness's status return was never read by a caller and would clash
 * with the real prototype if_urtwn.c already sees. */
void usbd_delay_ms(struct usbd_device *dev, unsigned int ms) {
	(void) dev;
	usb_osal_msleep(ms);
}

char *usbd_devinfo_alloc(struct usbd_device *dev, int showclass) {
	struct usbdi_dev *d;
	struct usbh_hubport *hport;
	char *buf;

	(void) showclass;
	if (dev == NULL) {
		return NULL;
	}
	d = usbdi_dv(dev);
	hport = d->hport;
	buf = wlan_kmalloc(128, M_WAITOK, M_USB);
	if (buf == NULL) {
		return NULL;
	}
	/* These dongles carry no readable product string, and naming one
	 * after the first driver that needed this looked like a different
	 * chip had attached: identify the device by its ids instead, the
	 * chip drivers announce their own names. */
	snprintf(buf, 128, "%04x:%04x, addr %d",
	    hport ? (unsigned) hport->device_desc.idVendor : 0,
	    hport ? (unsigned) hport->device_desc.idProduct : 0,
	    hport ? hport->dev_addr : 0);
	return buf;
}

void usbd_devinfo_free(char *devinfop) {
	if (devinfop != NULL) {
		wlan_kfree(devinfop, M_USB);
	}
}

void usbd_add_drv_event(int type, struct usbd_device *udev, device_t self) {
	(void) type;
	(void) udev;
	(void) self;
}

const char *usbd_errstr(usbd_status err) {
	switch (err) {
	case USBD_NORMAL_COMPLETION:
		return "no error";
	case USBD_IN_PROGRESS:
		return "io in progress";
	case USBD_NOT_STARTED:
		return "io not started";
	case USBD_INVAL:
		return "invalid argument";
	case USBD_NOMEM:
		return "out of memory";
	case USBD_CANCELLED:
		return "io cancelled";
	case USBD_IOERROR:
		return "usb io error";
	case USBD_NOT_CONFIGURED:
		return "device not configured";
	case USBD_STALLED:
		return "device stalled";
	case USBD_TIMEOUT:
		return "io timeout";
	default:
		return "unknown usb error";
	}
}

const struct usb_devno *usb_match_device(const struct usb_devno *tbl,
	u_int n, u_int entsize, uint16_t vendor, uint16_t product) {
	const struct usb_devno *ed = tbl;

	while (n-- != 0) {
		if (ed->ud_vendor == vendor &&
		    (ed->ud_product == product ||
		    ed->ud_product == USB_PRODUCT_ANY)) {
			return ed;
		}
		ed = (const struct usb_devno *)((const char *) ed + entsize);
	}
	return NULL;
}

/* wlan_cmd calls this right before if_init so the trace starts from
 * zero inside the driver init sequence */
void wlan_usbdi_trace_reset(void) {
	wlan_ctrl_trace_seq = 0;
	wlan_async_trace_seq = 0;
	wlan_trace_lvl = 0;
}

void wlan_usbdi_trace_set(unsigned level) {
	wlan_trace_lvl = level;
}

/* submit/complete reconciliation for `wlan usbstats`.  Clean run:
 *   worker == post_ok == complete - guard_drop + submit_fail
 *   submit == worker + (xfers still armed at the HCD) + (xfers still
 *   parked on pipe queues); q_peak <= 8 (urtwn tx pool per pipe),
 *   q_kicks + first-of-pipe == submits
 * A gap between complete and submit with guard_drop == 0 means the HCD
 * itself lost a completion (kill paths / IAA eat) - exactly what this
 * instrumentation is built to expose. */
void wlan_usbdi_stats_dump(void) {
	const struct wlan_usb_latency *tx = &wlan_usb_tx_latency;
	const struct wlan_usb_latency *rx = &wlan_usb_rx_latency;
	unsigned i;

	printf("[wlan] usbstats: submit=%u submit_fail=%u complete=%u "
	    "guard_drop=%u post_ok=%u post_drop=%u worker=%u kill=%u "
	    "peak_inflight=%u\n",
	    wlan_usb_stats.submit, wlan_usb_stats.submit_fail,
	    wlan_usb_stats.complete, wlan_usb_stats.guard_drop,
	    wlan_usb_stats.post_ok, wlan_usb_stats.post_drop,
	    wlan_usb_stats.worker_run, wlan_usb_stats.kill_calls,
	    wlan_usb_stats.in_flight_peak);
	printf("[wlan] usbstats: tx_submit=%u tx_complete=%u "
	    "rx_submit=%u rx_complete=%u wd_timeouts=%u\n",
	    wlan_usb_stats.tx_submit, wlan_usb_stats.tx_complete,
	    wlan_usb_stats.rx_submit, wlan_usb_stats.rx_complete,
	    wlan_usb_stats.wd_timeouts);
	printf("[wlan] usbstats: ctrl_fail=%u ctrl_retry=%u "
	    "stall_clear=%u task_drop=%u task_busy=%u\n",
	    wlan_usb_stats.ctrl_fail, wlan_usb_stats.ctrl_retry,
	    wlan_usb_stats.stall_clear,
	    wlan_usb_stats.task_drop, wlan_usb_stats.task_busy);
	printf("[wlan] usbstats: q_kicks=%u q_peak=%u q_flush=%u "
	    "q_timeout=%u q_orphan=%u\n",
	    wlan_usb_stats.q_kicks, wlan_usb_stats.q_peak,
	    wlan_usb_stats.q_flush, wlan_usb_stats.q_timeout,
	    wlan_usb_stats.q_orphan);
	printf("[wlan] usbtime tx: n=%u queue=%u/%u hcd=%u/%u wake=%u/%u ms "
	    "(sum/max)\n", tx->samples, tx->queue_total_ms, tx->queue_max_ms,
	    tx->hcd_total_ms, tx->hcd_max_ms, tx->wake_total_ms,
	    tx->wake_max_ms);
	printf("[wlan] usbtime rx: n=%u queue=%u/%u hcd=%u/%u wake=%u/%u ms "
	    "(sum/max)\n", rx->samples, rx->queue_total_ms, rx->queue_max_ms,
	    rx->hcd_total_ms, rx->hcd_max_ms, rx->wake_total_ms,
	    rx->wake_max_ms);

	/* per-device live state: how many xfers are armed at the HCD and how
	 * many are parked on pipe queues (they should account for every
	 * outstanding submit) */
	for (i = 0; i < s_usbdi_ndevs; i++) {
		struct usbdi_dev *d = s_usbdi_devs[i];
		struct usbdi_xfer *w;
		unsigned armed = 0;
		unsigned parked = 0;
		size_t flags = usbdi_ipl_save();

		SLIST_FOREACH(w, &d->in_flight_xfers, wd_next) {
			if (w->on_xq) {
				parked++;
			} else if (w->in_flight) {
				armed++;
			}
		}
		usbdi_ipl_restore(flags);
		printf("[wlan] usbdi%u: addr=%u speed=%u inflight_cnt=%u "
		    "armed=%u parked=%u ring=%u taskq=%u\n", i,
		    (unsigned) d->dev.ud_addr, (unsigned) d->dev.ud_speed,
		    d->in_flight_cnt, armed, parked,
		    (unsigned) (d->ring_tail - d->ring_head),
		    d->task_head);
	}
}

/* ------------------------------------------------------------------ */
/* usb task queues (NetBSD semantics: a queued task is not re-queued;
 * rem_task_wait blocks until the task has run or been removed) */

void usb_add_task(struct usbd_device *dev, struct usb_task *task,
	int queue) {
	struct usbdi_dev *d;
	size_t flags;

	(void) queue;
	if (dev == NULL || task == NULL) {
		return;
	}
	d = usbdi_dv(dev);

	flags = usbdi_ipl_save();
	if (task->queue != USB_NUM_TASKQS) {
		/* normal NetBSD behavior: a queued task is not re-queued */
		wlan_usb_stats.task_busy++;
		usbdi_ipl_restore(flags);
		return;
	}
	if (d->task_head >= USBD_SHIM_RING) {
		/* losing the add_task that carries a fresh driver cmdq
		 * entry wedges the command ring forever (its queued
		 * counter never returns to zero) - never stay silent */
		if (wlan_usb_stats.task_drop == 0) {
			printf("[wlan] usbdi task ring full: async work lost\n");
		}
		wlan_usb_stats.task_drop++;
		usbdi_ipl_restore(flags);
		return;
	}
	task->queue = (volatile unsigned) USB_TASKQ_DRIVER;
	d->tasks[d->task_head++] = task;
	usbdi_ipl_restore(flags);
	if (wlan_trace_lvl >= 1 && wlan_async_trace_seq < 48) {
		printf("[wlan] add_task fun=%p\n", task->fun);
		wlan_async_trace_seq++;
	}
	usb_osal_sem_give(d->task_sem);
}

bool usb_rem_task(struct usbd_device *dev, struct usb_task *task) {
	struct usbdi_dev *d;
	size_t flags;
	unsigned i;
	bool found = false;

	if (dev == NULL || task == NULL) {
		return false;
	}
	d = usbdi_dv(dev);
	flags = usbdi_ipl_save();
	for (i = 0; i < d->task_head; i++) {
		if (d->tasks[i] == task) {
			d->tasks[i] = NULL;
			task->queue = USB_NUM_TASKQS;
			found = true;
		}
	}
	usbdi_ipl_restore(flags);
	return found;
}

bool usb_rem_task_wait(struct usbd_device *dev, struct usb_task *task,
	int queue, kmutex_t *interlock) {
	struct usbdi_dev *d;

	(void) queue;
	(void) interlock; /* the detach call site passes NULL */

	if (dev == NULL || task == NULL) {
		return false;
	}
	d = usbdi_dv(dev);
	usb_rem_task(dev, task);
	mutex_enter(&d->wq_mtx);
	while (task->queue != USB_NUM_TASKQS) {
		cv_wait(&d->wq_cv, &d->wq_mtx);
	}
	mutex_exit(&d->wq_mtx);
	return true;
}

bool usb_task_pending(struct usbd_device *dev, struct usb_task *task) {
	(void) dev;
	if (task == NULL) {
		return false;
	}
	return task->queue != USB_NUM_TASKQS;
}

/* ------------------------------------------------------------------ */
/* workers */

/* Watchdog pass: recover PARKED xfers whose NetBSD xfer timeout expired
 * while waiting for the pipe (they never reached the HCD, so handing
 * USBD_TIMEOUT straight back is wire-side no-op and toggle-safe).
 *
 * Armed xfers are deliberately never killed.  A bulk OUT urb is stamped
 * at arm time with the toggle of the packet AFTER the full transfer and
 * the completion path writes that value back to the pipe for every
 * status, CANCELLED included.  A watchdog kill of a partially consumed
 * transfer therefore re-seeds the pipe with a toggle the device's
 * expected sequence does not hold (k vs k' consumed packets, odd parity
 * desyncs), bulk OUT has no other resync point, and the pipe is dead
 * forever - while leaving the qTD chain armed lets the controller keep
 * retrying; when the device wakes it consumes at its expected toggle
 * and the transfer completes correctly (the HCD has no timeout of its
 * own).  Same driver and firmware sustained 30 s @ 12.2 Mbit/s on
 * NetBSD, which has no such kill either.  Teardown paths (pipe
 * close/abort, xfer destroy) still kill: the pipes die right after. */
static void usbdi_watchdog_sweep(struct usbdi_dev *dev) {
	struct usbdi_xfer *overdue[8];
	unsigned stuck_ms[8];
	struct usbdi_xfer *w;
	unsigned now = wlan_usbdi_now_ms();
	unsigned i, n = 0;
	size_t flags;

	flags = usbdi_ipl_save();
	SLIST_FOREACH(w, &dev->in_flight_xfers, wd_next) {
		if (w->on_xq &&
		    w->wd_deadline != 0U &&
		    (int) (now - w->wd_deadline) >= 0) {
			/* clear the deadline so the next sweep cannot
			 * double-complete while the synthesized completion
			 * still sits in the ring */
			stuck_ms[n] = now - w->wd_deadline +
			    w->xfer.ux_timeout;
			w->wd_deadline = 0U;
			overdue[n++] = w;
			if (n == (sizeof(overdue) / sizeof(overdue[0]))) {
				break;
			}
		}
	}
	usbdi_ipl_restore(flags);

	for (i = 0; i < n; i++) {
		wlan_usb_stats.q_timeout++;
		wlan_port_serializer_lock();
		flags = usbdi_ipl_save();
		usbdi_xq_unlink(overdue[i]->pipe, overdue[i]);
		SLIST_REMOVE(&dev->in_flight_xfers, overdue[i], usbdi_xfer,
		    wd_next);
		overdue[i]->on_wd = 0;
		usbdi_ipl_restore(flags);
		overdue[i]->on_xq = 0;
		overdue[i]->xfer.ux_status = USBD_TIMEOUT;
		overdue[i]->xfer.ux_actlen = 0;
		if (wlan_trace_lvl >= 1) {
			printf("[wlan] usbdi xq timeout: %u ms parked "
			    "(timeout=%u)\n", stuck_ms[i],
			    overdue[i]->xfer.ux_timeout);
		}
		usbdi_ring_post(dev, &overdue[i]->xfer);
		wlan_port_serializer_unlock();
	}
}

static void usbdi_urb_worker_loop(void *arg) {
	struct usbdi_dev *dev = arg;

	while (dev->running) {
		struct usbd_xfer *xfer;
		unsigned head;

		/* bounded wait so the watchdog runs even with no
		 * completions arriving */
		(void) usb_osal_sem_take(dev->ring_sem, 500U);

		for (;;) {
			head = dev->ring_head;
			if (head == dev->ring_tail) {
				break;
			}
			xfer = dev->ring[head & USBD_SHIM_RING_MASK];
			__asm__ __volatile__("dmb ish" ::: "memory");
			dev->ring_head = head + 1;

			if (xfer == NULL ||
			    usbdi_xf(xfer)->magic != USBD_SHIM_MAGIC) {
				/* a corrupted entry must never reach the
				 * driver: count it and move on */
				wlan_usb_stats.guard_drop++;
				continue;
			}
			usbdi_urb_work(usbdi_xf(xfer));
		}

		usbdi_watchdog_sweep(dev);
	}
}

static void usbdi_taskq_worker_loop(void *arg) {
	struct usbdi_dev *dev = arg;

	while (dev->running) {
		struct usb_task *task;
		size_t flags;

		(void) usb_osal_sem_take(dev->task_sem,
		    USB_OSAL_WAITING_FOREVER);
		flags = usbdi_ipl_save();
		task = (dev->task_head != 0) ? dev->tasks[0] : NULL;
		if (task == NULL) {
			/* nothing queued, or usb_rem_task() already cleared
			 * the head slot; drop it and wait again */
			if (dev->task_head != 0) {
				memmove(&dev->tasks[0], &dev->tasks[1],
				    (dev->task_head - 1) * sizeof(task));
				dev->task_head--;
			}
			usbdi_ipl_restore(flags);
			continue;
		}
		memmove(&dev->tasks[0], &dev->tasks[1],
		    (dev->task_head - 1) * sizeof(task));
		dev->task_head--;
		usbdi_ipl_restore(flags);

		if (wlan_trace_lvl >= 1 && wlan_async_trace_seq < 48) {
			printf("[wlan] taskq run fun=%p\n", task->fun);
		}
		/* same splnet() discipline as the urb callbacks above: the
		 * state machine these tasks run sends management frames
		 * through the same tx queue the transmit path uses */
		wlan_port_serializer_lock();
		task->fun(task->arg);
		wlan_port_serializer_unlock();
		if (wlan_trace_lvl >= 1 && wlan_async_trace_seq < 48) {
			printf("[wlan] taskq done fun=%p\n", task->fun);
		}

		mutex_enter(&dev->wq_mtx);
		task->queue = USB_NUM_TASKQS;
		cv_broadcast(&dev->wq_cv);
		mutex_exit(&dev->wq_mtx);
	}
}

static void usbdi_workers_start(struct usbdi_dev *dev) {
	if (dev->workers_started) {
		return;
	}
	dev->workers_started = 1;
	dev->running = 1;
	dev->ring_sem = usb_osal_sem_create(0);
	dev->task_sem = usb_osal_sem_create(0);
	mutex_init(&dev->wq_mtx, MUTEX_DEFAULT, IPL_VM);
	cv_init(&dev->wq_cv, "usbdiwq");

	/* Two workers with their own stacks.  Not wlan_port_thread_create():
	 * that primitive's stack is the shared CONFIG_WLAN_WORKER_STACK
	 * (8 KiB, sized for the netbsd line's callout bodies), while the urb
	 * worker here runs the driver's completion path - urtwn_rxeof ->
	 * ieee80211_input, urtwn_txeof -> urtwn_start -> usbd_transfer - the
	 * deepest chain in the image.  The predecessors ran it on 32 KiB. */
	static const osThreadAttr_t urb_attr = {
		.name = "usbdi-urb",
		.priority = osPriorityBelowNormal,
		.stack_size = USBD_SHIM_URB_STACK,
	};
	static const osThreadAttr_t tq_attr = {
		.name = "usbdi-task",
		.priority = osPriorityBelowNormal,
		.stack_size = USBD_SHIM_TASKQ_STACK,
	};

	dev->urb_worker = (void *) osThreadNew(
	    (osThreadFunc_t) usbdi_urb_worker_loop, dev, &urb_attr);
	dev->taskq_worker = (void *) osThreadNew(
	    (osThreadFunc_t) usbdi_taskq_worker_loop, dev, &tq_attr);
	if (dev->urb_worker == NULL || dev->taskq_worker == NULL) {
		printf("[wlan] usbdi: worker thread create failed (%p/%p)\n",
		    dev->urb_worker, dev->taskq_worker);
	}
}

/* ------------------------------------------------------------------ */
/* attach: called by the cherryusb class hook when the dongle appears */

struct usbd_device *wlan_usbdi_attach(struct usbh_hubport *hport,
	void *drv_ctx) {
	struct usbdi_dev *d;

	if (hport == NULL || !hport->connected) {
		return NULL;
	}
	d = usbdi_dev_register(hport, drv_ctx);
	if (d == NULL) {
		printf("[wlan] usbdi: cannot build a device for %04x:%04x\n",
		    (unsigned) hport->device_desc.idVendor,
		    (unsigned) hport->device_desc.idProduct);
		return NULL;
	}
	usbdi_workers_start(d);

	if (s_usbdi_ndevs < USBD_SHIM_DEV_MAX) {
		s_usbdi_devs[s_usbdi_ndevs++] = d;
	}

	printf("[wlan] usbdi: %04x:%04x addr %u speed %u ifaces %u on "
	    "cherryusb\n",
	    (unsigned) hport->device_desc.idVendor,
	    (unsigned) hport->device_desc.idProduct,
	    (unsigned) d->dev.ud_addr, (unsigned) d->dev.ud_speed,
	    (unsigned) d->dev.ud_cdesc->bNumInterface);

	return &d->dev;
}

void wlan_usbdi_detach(struct usbd_device *dev) {
	struct usbdi_dev *d;

	if (dev == NULL) {
		return;
	}
	d = usbdi_dv(dev);
	d->running = 0;
	usb_osal_sem_give(d->ring_sem);
	usb_osal_sem_give(d->task_sem);
	/* the shim stays allocated: in-flight completions and the
	 * driver's xfers reference it, and this harness never
	 * re-attaches without a reboot */
}
