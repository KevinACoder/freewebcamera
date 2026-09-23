/*
 * @file
 * @brief NetBSD usbd_*(9) implementation over the CherryUSB host stack.
 *
 * Semantics (mirroring the FreeBSD/NetBSD usbd(9) shim pattern):
 *  - usbd_do_request -> usbh_control_transfer (blocking, 500 ms timeout
 *    inside cherryusb, 64-byte aligned bounce buffer);
 *  - usbd_transfer -> usbh_submit_urb with urb->timeout = 0, i.e. pure
 *    asynchronous per-URB QH; the NetBSD timeout watchdog stays with the
 *    driver (urtwn_watchdog -> usbd_abort_pipe);
 *  - the NetBSD per-pipe FIFO (up_queue) is reproduced here: the HCD
 *    keeps one urb per endpoint at a time, later xfers park on the pipe
 *    and the urb worker arms the next in submission order
 *    (usbd_pipe_kick) - armed urbs beyond the first are served
 *    newest-first by the EHCI async ring, and the device emits bulk
 *    frames in arrival order;
 *  - urb->complete runs in the EHCI interrupt handler, so it only drops
 *    the xfer into an IPL-protected ring and posts a semaphore; the
 *    driver callbacks run on the per-device worker kthread;
 *  - xfer buffers are 64-byte aligned (the driver exclusively uses
 *    usbd_get_buffer, so the shim owns all DMA memory);
 *  - descriptors come from the real cherryusb enumeration, not copies.
 *
 * @date 08.09.2026
 * @author zhugengyu
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <hal/ipl.h>
#include <mem/sysmalloc.h>

#include <usbh_core.h>
#include <usb_osal.h>

/* cherryusb's usb_def.h and NetBSD's dev/usb/usb.h disagree on the
 * numeric values of the speed codes above HIGH and on USB_MAX_DEVICES;
 * this unit lives in both worlds, so the cherryusb headers come first
 * and the NetBSD definitions win afterwards */
#undef USB_MAX_DEVICES
#undef USB_SPEED_LOW
#undef USB_SPEED_FULL
#undef USB_SPEED_HIGH
#undef USB_SPEED_WIRELESS
#undef USB_SPEED_SUPER
#undef USB_SPEED_SUPER_PLUS

#include <sys/queue.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdivar.h>
#include <port/port.h>
#include <port/bus/usb/port_usb.h>

#include "wlan_port_cherryusb.h"

/* embox thread entry points, provided by net_bridge.c */
extern void *wlan_port_thread_create(void *(*run)(void *), void *arg);
extern void wlan_port_thread_start(void *thread);
extern void ksleep(unsigned int ms);

#define USBD_SHIM_ALIGN 64
#define USBD_SHIFACE_MAX 4
#define USBD_SHIM_RING  64	/* power of two; >= 3x the 17 in-flight xfers */
#define USBD_SHIM_RING_MASK (USBD_SHIM_RING - 1)
#define USBD_SHIM_MAGIC 0xa510be55u

/* ------------------------------------------------------------------ */

struct usbd_interface {
	struct usbd_device *udev;
	uint8_t ifno;
};

struct usbd_device {
	struct usbh_hubport *hport;
	struct wlan_usb_dev *ud_port;
	usb_device_descriptor_t ddesc;
	struct usbd_interface ifaces[USBD_SHIFACE_MAX];
	volatile int dying;

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
	int running;

	/* xfers handed to the HCD and not yet dispatched to a driver
	 * callback (submit path inc, worker dec, both under ipl_save) */
	unsigned in_flight_cnt;

	/* all async xfers awaiting completion, for the watchdog sweep
	 * (submit inserts, worker removes, both under ipl_save) */
	SLIST_HEAD(, usbd_xfer) in_flight_xfers;
};

struct usbd_pipe {
	struct usbd_device *dev;
	struct usb_endpoint_descriptor *cherry_ep; /* inside hport config */
	usb_endpoint_descriptor_t ed;              /* NetBSD view */
	uint8_t data_toggle;
	SLIST_HEAD(, usbd_xfer) pending;
	/* NetBSD up_queue: xfers parked while the pipe's slot at the HCD
	 * is taken.  The cherryusb host contract is one urb per endpoint
	 * in flight; armed xfers beyond the first are served newest-first
	 * by the async ring and the device emits bulk frames in arrival
	 * order, so ordering is enforced here instead.  All xq state is
	 * mutated under the port serializer (callers hold it around
	 * usbd_transfer; the worker takes it in usbd_pipe_kick). */
	SLIST_HEAD(, usbd_xfer) xq;
	struct usbd_xfer *xq_tail;
	int xq_busy;	/* an armed xfer holds the pipe */
	unsigned xq_len;
};

struct usbd_xfer {
	struct usbd_pipe *pipe;
	void *priv;
	void *buffer;
	uint32_t length;
	uint16_t flags;
	uint32_t timeout;
	usbd_callback callback;
	void *dma_buf;
	usbd_status status;
	uint32_t actlen;
	unsigned submit_ms;
	unsigned arm_ms;
	unsigned done_ms;
	volatile int in_flight;
	int in_xq;	/* parked on pipe->xq, not yet at the HCD */
	void *dma_raw;	/* the block dma_buf points inside of; the free
			 * must go to this one - vPortFree on the aligned
			 * interior point reads a phantom heap header and
			 * steers coalescing into live neighbors (M7) */
	unsigned wd_deadline;	/* watchdog deadline (wlan_port_now_ms);
				 * 0 = unarmed (USBD_NO_TIMEOUT) */
	unsigned magic;		/* USBD_SHIM_MAGIC, checked on ring dequeue */
	struct usbh_urb urb;
	SLIST_ENTRY(usbd_xfer) next;
	SLIST_ENTRY(usbd_xfer) wd_next;
	SLIST_ENTRY(usbd_xfer) xq_next;
};

/* control bounce region: EHCI requires 64-byte aligned setup/data while
 * the driver buffers live on the stack or in the softc */
static struct {
	struct usb_setup_packet setup;
	uint8_t pad[64 - sizeof(struct usb_setup_packet)];
	uint8_t buf[512];
} __attribute__((aligned(64))) s_ctrl;
static kmutex_t s_ctrl_mtx;
static int s_shim_ready;

static void shim_locks_init(void) {
	if (s_shim_ready) {
		return;
	}
	s_shim_ready = 1;
	mutex_init(&s_ctrl_mtx, MUTEX_DEFAULT, IPL_USB);
}

static usbd_status usbd_map_err(int cherry_err) {
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
	const struct usbd_xfer *xfer, unsigned now_ms) {
	unsigned queue_ms = xfer->arm_ms - xfer->submit_ms;
	unsigned hcd_ms = xfer->done_ms - xfer->arm_ms;
	unsigned wake_ms = now_ms - xfer->done_ms;

	lat->samples++;
	lat->queue_total_ms += queue_ms;
	lat->hcd_total_ms += hcd_ms;
	lat->wake_total_ms += wake_ms;
	if (queue_ms > lat->queue_max_ms) lat->queue_max_ms = queue_ms;
	if (hcd_ms > lat->hcd_max_ms) lat->hcd_max_ms = hcd_ms;
	if (wake_ms > lat->wake_max_ms) lat->wake_max_ms = wake_ms;
}

void wlan_usbdi_stats_dump(void);

static int usbd_pipe_is_tx(const struct usbd_pipe *pipe) {
	return (pipe != NULL && (pipe->ed.bEndpointAddress & 0x80U) == 0U);
}

static void usbd_ring_post(struct usbd_device *dev, struct usbd_xfer *xfer) {
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

/* remove a parked xfer from its pipe queue; callers hold the serializer */
static void usbd_xq_unlink(struct usbd_pipe *pipe, struct usbd_xfer *xfer) {
	struct usbd_xfer *cur, *prev = NULL;

	SLIST_FOREACH(cur, &pipe->xq, xq_next) {
		if (cur == xfer) {
			if (prev == NULL) {
				SLIST_REMOVE_HEAD(&pipe->xq, xq_next);
			} else {
				SLIST_REMOVE_AFTER(prev, xq_next);
			}
			if (pipe->xq_tail == xfer) {
				pipe->xq_tail = prev;
			}
			if (pipe->xq_len > 0U) {
				pipe->xq_len--;
			}
			return;
		}
		prev = cur;
	}
}

/* return every parked xfer to the driver with one status, without
 * touching the HCD (they never reached it).  Completions are posted to
 * the ring so the callbacks still run on the urb worker.  Callers hold
 * the serializer. */
static void usbd_xq_flush(struct usbd_pipe *pipe, usbd_status status) {
	struct usbd_xfer *xfer;

	while ((xfer = SLIST_FIRST(&pipe->xq)) != NULL) {
		SLIST_REMOVE_HEAD(&pipe->xq, xq_next);
		if (SLIST_FIRST(&pipe->xq) == NULL) {
			pipe->xq_tail = NULL;
		}
		pipe->xq_len--;
		xfer->in_xq = 0;
		xfer->wd_deadline = 0U;
		xfer->status = status;
		xfer->actlen = 0;
		wlan_usb_stats.q_flush++;
		usbd_ring_post(pipe->dev, xfer);
	}
}

/* ------------------------------------------------------------------ */
/* device/interface shells */

static struct usbd_device *usbd_shim_register_device(struct usbh_hubport *hport,
	struct wlan_usb_dev *port_dev) {
	struct usbd_device *dev;
	uint8_t i;

	dev = wlan_kmalloc(sizeof(*dev), M_WAITOK | M_ZERO, M_USB);
	if (dev == NULL) {
		return NULL;
	}
	dev->hport = hport;
	dev->ud_port = port_dev;
	memcpy(&dev->ddesc, &hport->device_desc, sizeof(dev->ddesc));
	for (i = 0; i < USBD_SHIFACE_MAX; i++) {
		dev->ifaces[i].udev = dev;
		dev->ifaces[i].ifno = i;
	}
	return dev;
}

usb_device_descriptor_t *usbd_get_device_descriptor(struct usbd_device *dev) {
	return &dev->ddesc;
}

usb_interface_descriptor_t *usbd_get_interface_descriptor(
	struct usbd_interface *iface) {
	struct usbh_interface_altsetting *alt;

	if (iface == NULL || iface->udev->hport == NULL) {
		return NULL;
	}
	alt = &iface->udev->hport->config.intf[iface->ifno].altsetting[0];
	return (usb_interface_descriptor_t *) &alt->intf_desc;
}

usb_endpoint_descriptor_t *usbd_interface2endpoint_descriptor(
	struct usbd_interface *iface, uint8_t index) {
	struct usbh_interface_altsetting *alt;

	if (iface == NULL || iface->udev->hport == NULL) {
		return NULL;
	}
	alt = &iface->udev->hport->config.intf[iface->ifno].altsetting[0];
	if (index >= alt->intf_desc.bNumEndpoints ||
		index >= CONFIG_USBHOST_MAX_ENDPOINTS) {
		return NULL;
	}
	return (usb_endpoint_descriptor_t *) (void *) &alt->ep[index].ep_desc;
}

void usbd_interface2device_handle(struct usbd_interface *iface,
	struct usbd_device **dev) {
	*dev = iface->udev;
}

usbd_status usbd_device2interface_handle(struct usbd_device *dev,
	uint8_t ifindex, struct usbd_interface **iface) {
	if (dev == NULL || ifindex >= USBD_SHIFACE_MAX) {
		return USBD_INVAL;
	}
	*iface = &dev->ifaces[ifindex];
	return USBD_NORMAL_COMPLETION;
}

/* ------------------------------------------------------------------ */

usbd_status usbd_open_pipe(struct usbd_interface *iface, uint8_t address,
	uint8_t flags, struct usbd_pipe **pipep) {
	struct usbd_device *dev;
	struct usbd_pipe *pipe;
	usb_interface_descriptor_t *id;
	usb_endpoint_descriptor_t *ed;
	uint8_t i;

	(void) flags; /* per-URB QH: exclusivity is inherent */

	if (iface == NULL) {
		return USBD_INVAL;
	}
	dev = iface->udev;

	pipe = wlan_kmalloc(sizeof(*pipe), M_WAITOK | M_ZERO, M_USB);
	if (pipe == NULL) {
		return USBD_NOMEM;
	}
	pipe->dev = dev;
	SLIST_INIT(&pipe->pending);
	SLIST_INIT(&pipe->xq);

	id = usbd_get_interface_descriptor(iface);
	for (i = 0;; i++) {
		ed = usbd_interface2endpoint_descriptor(iface, i);
		if (ed == NULL || (id != NULL && i > id->bNumEndpoints)) {
			ed = NULL;
			break;
		}
		if (ed->bEndpointAddress == address) {
			break;
		}
	}
	if (ed == NULL) {
		wlan_kfree(pipe, M_USB);
		return USBD_INVAL;
	}
	pipe->ed = *ed;
	pipe->cherry_ep = (struct usb_endpoint_descriptor *) (void *) ed;
	*pipep = pipe;
	return USBD_NORMAL_COMPLETION;
}

void usbd_close_pipe(struct usbd_pipe *pipe) {
	struct usbd_xfer *xfer;
	ipl_t ipl;

	if (pipe == NULL) {
		return;
	}
	wlan_port_serializer_lock();
	usbd_xq_flush(pipe, USBD_CANCELLED);
	wlan_port_serializer_unlock();
	ipl = ipl_save();
	SLIST_FOREACH(xfer, &pipe->pending, next) {
		if (xfer->in_flight) {
			wlan_usb_stats.kill_calls++;
			(void) usbh_kill_urb(&xfer->urb);
		}
	}
	ipl_restore(ipl);
	wlan_kfree(pipe, M_USB);
}

void usbd_abort_pipe(struct usbd_pipe *pipe) {
	struct usbd_xfer *xfer;
	ipl_t ipl;

	if (pipe == NULL) {
		return;
	}
	/* NetBSD aborts the whole pipe: the queued xfers never reached the
	 * HCD, so they come straight back CANCELLED; the armed one is
	 * killed below and its completion kicks the (now empty) queue */
	wlan_port_serializer_lock();
	usbd_xq_flush(pipe, USBD_CANCELLED);
	wlan_port_serializer_unlock();
	ipl = ipl_save();
	SLIST_FOREACH(xfer, &pipe->pending, next) {
		if (xfer->in_flight) {
			/* thread context only (watchdog / stop paths) */
			wlan_usb_stats.kill_calls++;
			(void) usbh_kill_urb(&xfer->urb);
		}
	}
	ipl_restore(ipl);
}

static usbd_status usbd_pipe_clear_halt(struct usbd_pipe *pipe);
void usbd_clear_endpoint_stall_async(struct usbd_pipe *pipe) {
	(void) usbd_pipe_clear_halt(pipe);
}

/* ------------------------------------------------------------------ */

int usbd_create_xfer(struct usbd_pipe *pipe, size_t size, unsigned int flags,
	unsigned int nframes, struct usbd_xfer **xp) {
	struct usbd_xfer *xfer;

	(void) nframes;
	xfer = wlan_kmalloc(sizeof(*xfer), M_WAITOK | M_ZERO, M_USB);
	if (xfer == NULL) {
		return USBD_NOMEM;
	}
	xfer->pipe = pipe;
	xfer->flags = (uint16_t) flags;
	xfer->magic = USBD_SHIM_MAGIC;
	if (size != 0) {
		/* sysmemalign hands back an interior point of a larger block
		 * and drops the raw one - freeing that pointer corrupts the
		 * heap. Do the alignment dance here and keep the raw owner
		 * for the free; the M_ZERO covers the old explicit memset
		 * of the window the driver sees. */
		xfer->dma_raw = wlan_kmalloc(size + USBD_SHIM_ALIGN,
		    M_WAITOK | M_ZERO, M_USB);
		if (xfer->dma_raw == NULL) {
			wlan_kfree(xfer, M_USB);
			return USBD_NOMEM;
		}
		xfer->dma_buf = (void *)(((uintptr_t) xfer->dma_raw +
		    USBD_SHIM_ALIGN - 1) & ~(uintptr_t)(USBD_SHIM_ALIGN - 1));
	}
	*xp = xfer;
	return USBD_NORMAL_COMPLETION;
}

void usbd_destroy_xfer(struct usbd_xfer *xfer) {
	ipl_t ipl;

	if (xfer == NULL) {
		return;
	}
	if (xfer->in_xq && xfer->pipe != NULL) {
		/* a parked xfer must have been flushed or completed before
		 * the driver destroys it; unlink defensively and stay loud */
		wlan_usb_stats.q_orphan++;
		wlan_port_serializer_lock();
		ipl = ipl_save();
		usbd_xq_unlink(xfer->pipe, xfer);
		SLIST_REMOVE(&xfer->pipe->dev->in_flight_xfers, xfer,
		    usbd_xfer, wd_next);
		ipl_restore(ipl);
		wlan_port_serializer_unlock();
		xfer->in_xq = 0;
	}
	if (xfer->in_flight) {
		wlan_usb_stats.kill_calls++;
		(void) usbh_kill_urb(&xfer->urb);
	}
	if (xfer->dma_raw != NULL) {
		wlan_kfree(xfer->dma_raw, M_USB);
	}
	wlan_kfree(xfer, M_USB);
}

void *usbd_get_buffer(struct usbd_xfer *xfer) {
	return xfer->dma_buf;
}

void usbd_setup_xfer(struct usbd_xfer *xfer, void *priv, void *buffer,
	uint32_t length, uint16_t flags, uint32_t timeout, usbd_callback cb) {
	xfer->priv = priv;
	xfer->buffer = (buffer != NULL) ? buffer : xfer->dma_buf;
	xfer->length = length;
	xfer->flags = flags;
	xfer->timeout = timeout;
	xfer->callback = cb;
	xfer->status = USBD_NOT_STARTED;
	xfer->actlen = 0;
}

void usbd_get_xfer_status(struct usbd_xfer *xfer, void **priv, void **buffer,
	uint32_t *actlen, usbd_status *status) {
	if (priv != NULL) {
		*priv = xfer->priv;
	}
	if (buffer != NULL) {
		*buffer = xfer->buffer;
	}
	if (actlen != NULL) {
		*actlen = xfer->actlen;
	}
	if (status != NULL) {
		*status = xfer->status;
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
static void usbd_shim_urb_complete(void *arg, int nbytes_or_err) {
	struct usbd_xfer *xfer = arg;
	struct usbd_device *dev;

	wlan_usb_stats.complete++;

	/* The async completion and the worker (which re-submits the xfer)
	 * race on the same xfer.  Only accept the completion while the
	 * transfer is still in flight; once the worker took it off the
	 * pending list and re-armed it, a stale completion must not touch
	 * it again. */
	if (xfer == NULL || !xfer->in_flight || xfer->pipe == NULL) {
		wlan_usb_stats.guard_drop++;
		return;
	}
	dev = xfer->pipe->dev;
	xfer->done_ms = wlan_port_now_ms();
	xfer->pipe->data_toggle = xfer->urb.data_toggle;

	if (nbytes_or_err < 0) {
		xfer->status = usbd_map_err(nbytes_or_err);
		xfer->actlen = 0;
	} else {
		xfer->status = USBD_NORMAL_COMPLETION;
		xfer->actlen = (uint32_t) nbytes_or_err;
	}

	if (dev != NULL) {
		usbd_ring_post(dev, xfer);
	}
}

/* fill the urb from pipe state and hand the transfer to the HCD; the
 * xfer is already on the device watchdog list and carries an armed
 * deadline from its usbd_transfer call.  On submit failure the
 * completion is synthesized onto the ring (the driver still gets its
 * callback) and the cherry error code is returned so the caller can
 * release the pipe for the next queued xfer. */
static int usbd_pipe_arm(struct usbd_xfer *xfer) {
	struct usbd_device *dev = xfer->pipe->dev;
	ipl_t ipl;
	int ret;

	memset(&xfer->urb, 0, sizeof(xfer->urb));
	/* seed from the pipe: with one urb in flight per pipe this read
	 * always happens after the previous completion wrote the toggle
	 * back (usbd_shim_urb_complete), or after clear-halt reset it */
	xfer->urb.data_toggle = xfer->pipe->data_toggle;
	usbh_bulk_urb_fill(&xfer->urb, dev->hport, xfer->pipe->cherry_ep,
	    xfer->buffer, xfer->length,
	    0 /* timeout=0: asynchronous */, usbd_shim_urb_complete, xfer);
	{
		/* USBD_FORCE_SHORT_XFER: bulk OUT of an exact multiple of
		 * the max packet size terminates with a zero-length
		 * packet (the HCD appends it from this flag); without it
		 * the device-side bulk FIFO can hold the last full packet
		 * waiting for a short-packet delimiter */
		uint16_t mps = USB_GET_MAXPACKETSIZE(xfer->pipe->cherry_ep->wMaxPacketSize);

		if ((xfer->flags & USBD_FORCE_SHORT_XFER) != 0U &&
		    xfer->length != 0U && mps != 0U &&
		    (xfer->length % mps) == 0U) {
			xfer->urb.transfer_flags = USBH_URB_ZERO_PACKET;
		}
	}

	xfer->in_flight = 1;
	xfer->arm_ms = wlan_port_now_ms();
	if (usbd_pipe_is_tx(xfer->pipe)) {
		wlan_usb_stats.tx_submit++;
	} else {
		wlan_usb_stats.rx_submit++;
	}
	ipl = ipl_save();
	SLIST_INSERT_HEAD(&xfer->pipe->pending, xfer, next);
	dev->in_flight_cnt++;
	if (dev->in_flight_cnt > wlan_usb_stats.in_flight_peak) {
		wlan_usb_stats.in_flight_peak = dev->in_flight_cnt;
	}
	ipl_restore(ipl);
	{
		static unsigned urb_subs;

		if (wlan_trace_lvl >= 1 && urb_subs < 48) {
			printf("[wlan] urb submit #%u: ep=%02x len=%u\n",
			    urb_subs, xfer->pipe->ed.bEndpointAddress,
			    xfer->length);
		}
		urb_subs++;
	}

	if (dev->hport == NULL) {
		ret = -USB_ERR_NOTCONN;
	} else {
		ret = usbh_submit_urb(&xfer->urb);
	}
	if (ret != 0) {
		/* not connected / busy: synthesize the callback so the
		 * driver can reclaim its tx_data */
		wlan_usb_stats.submit_fail++;
		xfer->in_flight = 0;
		xfer->wd_deadline = 0U;
		ipl = ipl_save();
		SLIST_REMOVE(&dev->in_flight_xfers, xfer, usbd_xfer, wd_next);
		ipl_restore(ipl);
		xfer->status = usbd_map_err(ret);
		xfer->done_ms = wlan_port_now_ms();
		usbd_ring_post(dev, xfer);
	}
	return ret;
}

/* arm the next parked xfer, in submission order; called from the urb
 * worker when an armed transfer retires (the shape of NetBSD's HCD
 * softint restarting the pipe).  Holds the serializer so thread-side
 * usbd_transfer callers queueing behind stay exclusive.  The loop only
 * continues past a failed arm: a successful one owns the pipe again. */
static void usbd_pipe_kick(struct usbd_pipe *pipe) {
	struct usbd_xfer *xfer;
	ipl_t ipl;
	int ret;

	wlan_port_serializer_lock();
	for (;;) {
		ipl = ipl_save();
		xfer = SLIST_FIRST(&pipe->xq);
		if (xfer == NULL) {
			pipe->xq_busy = 0;
			ipl_restore(ipl);
			break;
		}
		SLIST_REMOVE_HEAD(&pipe->xq, xq_next);
		if (SLIST_FIRST(&pipe->xq) == NULL) {
			pipe->xq_tail = NULL;
		}
		pipe->xq_len--;
		xfer->in_xq = 0;
		ipl_restore(ipl);
		wlan_usb_stats.q_kicks++;
		ret = usbd_pipe_arm(xfer);
		if (ret == 0) {
			break;
		}
	}
	wlan_port_serializer_unlock();
}

/* worker context: unlink from the pipe, then run the driver callback */
static void usbd_shim_urb_work(struct usbd_xfer *xfer) {
	usbd_callback cb;
	void *priv;
	usbd_status status;
	ipl_t ipl;
	int was_armed;

	if (wlan_trace_lvl >= 1 && wlan_async_trace_seq < 48) {
		printf("[wlan] urb done: status=%d actlen=%u\n",
		    (int) xfer->status, xfer->actlen);
	}

	was_armed = xfer->in_flight;
	if (was_armed && xfer->status == USBD_NORMAL_COMPLETION) {
		wlan_usb_latency_record(usbd_pipe_is_tx(xfer->pipe)
		    ? &wlan_usb_tx_latency : &wlan_usb_rx_latency,
		    xfer, wlan_port_now_ms());
	}
	xfer->in_flight = 0;
	xfer->wd_deadline = 0U;
	if (xfer->pipe != NULL) {
		ipl = ipl_save();
		SLIST_REMOVE(&xfer->pipe->pending, xfer, usbd_xfer, next);
		SLIST_REMOVE(&xfer->pipe->dev->in_flight_xfers, xfer,
		    usbd_xfer, wd_next);
		if (xfer->pipe->dev->in_flight_cnt > 0U) {
			xfer->pipe->dev->in_flight_cnt--;
		}
		ipl_restore(ipl);
	}
	wlan_usb_stats.worker_run++;
	if (usbd_pipe_is_tx(xfer->pipe)) {
		wlan_usb_stats.tx_complete++;
	} else {
		wlan_usb_stats.rx_complete++;
	}

	cb = xfer->callback;
	priv = xfer->priv;
	status = xfer->status;
	if (status == USBD_STALLED && xfer->pipe != NULL &&
	    (xfer->pipe->ed.bmAttributes & 0x03U) == USB_ENDPOINT_TYPE_BULK) {
		/* the HCD never recovers a halted bulk endpoint on its
		 * own; clear the halt so the driver's re-arm actually
		 * reaches the device instead of completing STALLED
		 * forever.  This doubles as the queue's stall gate: the
		 * parked xfers arm only after the endpoint is walking
		 * again and the pipe toggle is back to DATA0. */
		(void) usbd_pipe_clear_halt(xfer->pipe);
	}
	if (was_armed && xfer->pipe != NULL) {
		/* only an armed completion frees the pipe's slot at the
		 * HCD (a parked xfer returning by timeout/flush does not);
		 * start the next queued xfer before the driver callback
		 * so the wire keeps flowing while it runs */
		usbd_pipe_kick(xfer->pipe);
	}
	if (cb != NULL) {
		/* Run the driver callback under the port serializer, the
		 * port's replacement for the splnet() discipline of the
		 * imported NetBSD code: the driver completion path
		 * (urtwn_txeof -> urtwn_start) mutates the same tx queue
		 * and free list as the transmit path (wlan_port_xmit_urtwn
		 * -> if_start_lock), and on an SMP kernel two cores can be
		 * inside both concurrently unless they exclude here. The
		 * serializer is reentrant and tsleep drops it around
		 * waits, so nested driver entry stays correct. */
		wlan_port_serializer_lock();
		cb(xfer, priv, status);
		wlan_port_serializer_unlock();
	}
}

usbd_status usbd_transfer(struct usbd_xfer *xfer) {
	struct usbd_device *dev;
	struct usbd_pipe *pipe;
	ipl_t ipl;

	if (xfer->pipe == NULL) {
		return USBD_INVAL;
	}
	pipe = xfer->pipe;
	dev = pipe->dev;
	if (dev->hport == NULL) {
		return USBD_IOERROR;
	}
	shim_locks_init();

	wlan_usb_stats.submit++;
	xfer->submit_ms = wlan_port_now_ms();

	/* NetBSD usbdi serves every pipe strictly FIFO (up_queue) while
	 * the cherryusb HCD contract is one urb per endpoint in flight:
	 * the EHCI port arms a fresh QH at the async ring head per urb,
	 * so several armed urbs would be served newest-first and the
	 * device emits bulk frames in arrival order.  While the pipe's
	 * slot at the HCD is taken, park here and let the completion
	 * worker arm us in submission order (usbd_pipe_kick).  Callers
	 * hold the port serializer (the splnet() discipline), which makes
	 * this exclusive against the worker's kick. */
	ipl = ipl_save();
	SLIST_INSERT_HEAD(&dev->in_flight_xfers, xfer, wd_next);
	xfer->status = USBD_IN_PROGRESS;
	xfer->actlen = 0;
	xfer->wd_deadline = (xfer->timeout != 0U)
	    ? wlan_port_now_ms() + xfer->timeout : 0U;
	if (pipe->xq_busy) {
		if (pipe->xq_tail != NULL) {
			SLIST_INSERT_AFTER(pipe->xq_tail, xfer, xq_next);
		} else {
			SLIST_INSERT_HEAD(&pipe->xq, xfer, xq_next);
		}
		pipe->xq_tail = xfer;
		pipe->xq_len++;
		if (pipe->xq_len > wlan_usb_stats.q_peak) {
			wlan_usb_stats.q_peak = pipe->xq_len;
		}
		xfer->in_xq = 1;
		ipl_restore(ipl);
		return USBD_IN_PROGRESS;
	}
	pipe->xq_busy = 1;
	ipl_restore(ipl);

	if (usbd_pipe_arm(xfer) != 0) {
		/* submit failed: the synthesized completion is already on
		 * its way through the ring and nothing is armed on this
		 * pipe, so the slot is free again right now */
		ipl = ipl_save();
		pipe->xq_busy = 0;
		ipl_restore(ipl);
	}
	return USBD_IN_PROGRESS;
}

/* ------------------------------------------------------------------ */
/* control transfers */

static usbd_status usbd_ctrl_xfer(struct usbd_device *dev,
	usb_device_request_t *req, void *data, int *actlen);

/* bulk STALL recovery: CLEAR_FEATURE(ENDPOINT_HALT) returns both sides
 * to DATA0.  Nothing in cherryusb ever sent one (its clear-feature paths
 * only cover hub port features), so a halted bulk endpoint used to stay
 * dead forever; the completion worker and the driver's
 * usbd_clear_endpoint_stall_async both come through here. */
static usbd_status usbd_pipe_clear_halt(struct usbd_pipe *pipe) {
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
	err = usbd_ctrl_xfer(pipe->dev, &req, NULL, NULL);
	if (err == USBD_NORMAL_COMPLETION) {
		/* clear-halt resets the endpoint toggle to DATA0 on both
		 * sides; the EHCI port takes its next start toggle from
		 * the pipe (bulk OUT) or resyncs from the wire (bulk IN) */
		pipe->data_toggle = 0;
		wlan_usb_stats.stall_clear++;
	}
	return err;
}
/* control-transfer trace counter, reset by wlan_usbdi_trace_reset() */
static unsigned wlan_ctrl_trace_seq;
void wlan_usbdi_trace_reset(void);
void wlan_usbdi_trace_set(unsigned level);

static usbd_status usbd_ctrl_xfer(struct usbd_device *dev,
	usb_device_request_t *req, void *data, int *actlen) {
	uint16_t len;
	int is_read;
	int ret;

	if (dev->hport == NULL) {
		return USBD_IOERROR;
	}
	shim_locks_init();
	mutex_enter(&s_ctrl_mtx);

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
	len = (uint16_t) UGETW(req->wLength);
	s_ctrl.setup.wLength = len;

	is_read = (req->bmRequestType & 0x80U) != 0U;
	if (len > 0 && !is_read && data != NULL) {
		memcpy(s_ctrl.buf, data, len);
	}

	/* The device occasionally leaves ep0 unanswered for longer than
	 * usbh's fixed 500 ms control timeout (observed on LED/calib reads
	 * during scanning).  CherryUSB kills the timed-out URB, so a fresh
	 * submission is safe; give it a few chances before failing. */
	for (unsigned attempt = 0;; attempt++) {
		ret = usbh_control_transfer(dev->hport, &s_ctrl.setup,
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
		return usbd_map_err(ret);
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
	return usbd_ctrl_xfer(dev, req, data, NULL);
}

usbd_status usbd_do_request_flags(struct usbd_device *dev,
	usb_device_request_t *req, void *data, uint16_t flags, int *actlen,
	uint32_t timeout) {
	(void) flags;
	(void) timeout; /* usbh_control_transfer: fixed 500 ms inside */
	return usbd_ctrl_xfer(dev, req, data, actlen);
}

usbd_status usbd_set_config_no(struct usbd_device *dev, int config,
	int flags) {
	usb_device_request_t req;
	uint8_t val = 0;
	usbd_status err;

	(void) flags;
	if (dev->hport == NULL) {
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

usbd_status usbd_delay_ms(struct usbd_device *dev, unsigned int ms) {
	(void) dev;
	ksleep(ms);
	return USBD_NORMAL_COMPLETION;
}

char *usbd_devinfo_alloc(struct usbd_device *dev, int showclass) {
	struct usbh_hubport *hport = dev->hport;
	const char *vend;
	const char *prod;
	char *buf;

	(void) showclass;
	vend = (hport && hport->iManufacturer) ? hport->iManufacturer : "Realtek";
	prod = (hport && hport->iProduct) ? hport->iProduct : "RTL8188EU";
	buf = wlan_kmalloc(128, M_WAITOK, M_USB);
	if (buf == NULL) {
		return NULL;
	}
	snprintf(buf, 128, "%s %s, addr %d", vend, prod,
	    hport ? hport->dev_addr : 0);
	return buf;
}

void usbd_devinfo_free(char *devinfop) {
	wlan_kfree(devinfop, M_USB);
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
		ed = (const struct usb_devno *)((const char *)ed + entsize);
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
 *   q_kicks + tx_submit first-of-pipe == tx_submit total
 * A gap between complete and submit with guard_drop == 0 means the HCD
 * itself lost a completion (kill paths / IAA eat) - exactly what this
 * instrumentation is built to expose. */
void wlan_usbdi_stats_dump(void) {
	const struct wlan_usb_latency *tx = &wlan_usb_tx_latency;
	const struct wlan_usb_latency *rx = &wlan_usb_rx_latency;
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
	printf("[wlan] usbtime tx: n=%u queue=%u/%u hcd=%u/%u wake=%u/%u ms (sum/max)\n",
	    tx->samples, tx->queue_total_ms, tx->queue_max_ms,
	    tx->hcd_total_ms, tx->hcd_max_ms, tx->wake_total_ms,
	    tx->wake_max_ms);
	printf("[wlan] usbtime rx: n=%u queue=%u/%u hcd=%u/%u wake=%u/%u ms (sum/max)\n",
	    rx->samples, rx->queue_total_ms, rx->queue_max_ms,
	    rx->hcd_total_ms, rx->hcd_max_ms, rx->wake_total_ms,
	    rx->wake_max_ms);
}

/* ------------------------------------------------------------------ */
/* usb task queues (NetBSD semantics: a queued task is not re-queued;
 * rem_task_wait blocks until the task has run or been removed) */

void usb_add_task(struct usbd_device *dev, struct usb_task *task,
	int queue) {
	ipl_t ipl;

	(void) queue;
	if (dev == NULL) {
		return;
	}

	ipl = ipl_save();
	if (task->queue != USB_NUM_TASKQS) {
		/* normal NetBSD behavior: a queued task is not re-queued */
		wlan_usb_stats.task_busy++;
		ipl_restore(ipl);
		return;
	}
	if (dev->task_head >= USBD_SHIM_RING) {
		/* losing the add_task that carries a fresh driver cmdq
		 * entry wedges the command ring forever (its queued
		 * counter never returns to zero) - never stay silent */
		if (wlan_usb_stats.task_drop == 0) {
			printf("[wlan] usbdi task ring full: async work lost\n");
		}
		wlan_usb_stats.task_drop++;
		ipl_restore(ipl);
		return;
	}
	task->queue = (volatile unsigned) USB_TASKQ_DRIVER;
	dev->tasks[dev->task_head++] = task;
	ipl_restore(ipl);
	if (wlan_trace_lvl >= 1 && wlan_async_trace_seq < 48) {
		printf("[wlan] add_task fun=%p\n", task->fun);
		wlan_async_trace_seq++;
	}
	usb_osal_sem_give(dev->task_sem);
}

bool usb_rem_task(struct usbd_device *dev, struct usb_task *task) {
	ipl_t ipl;
	unsigned i;
	bool found = false;

	if (dev == NULL) {
		return false;
	}
	ipl = ipl_save();
	for (i = 0; i < dev->task_head; i++) {
		if (dev->tasks[i] == task) {
			dev->tasks[i] = NULL;
			task->queue = USB_NUM_TASKQS;
			found = true;
		}
	}
	ipl_restore(ipl);
	return found;
}

bool usb_rem_task_wait(struct usbd_device *dev, struct usb_task *task,
	int queue, kmutex_t *interlock) {
	(void) queue;
	(void) interlock; /* the detach call site passes NULL */

	if (dev == NULL) {
		return false;
	}
	usb_rem_task(dev, task);
	mutex_enter(&dev->wq_mtx);
	while (task->queue != USB_NUM_TASKQS) {
		cv_wait(&dev->wq_cv, &dev->wq_mtx);
	}
	mutex_exit(&dev->wq_mtx);
	return true;
}

bool usb_task_pending(struct usbd_device *dev, struct usb_task *task) {
	(void) dev;
	return task->queue != USB_NUM_TASKQS;
}

/* ------------------------------------------------------------------ */
/* workers */

/* Watchdog pass: recover xfers whose NetBSD xfer timeout (armed at
 * submit, 5 s for urtwn TX) expired while in flight.  NetBSD's
 * usbd_xfer_timeout aborts the transfer and delivers USBD_TIMEOUT to
 * the driver callback once; without this the shim had no recovery at
 * all and one lost transfer leaked its driver buffer forever.  Killed
 * xfers come back through urb->complete with -USB_ERR_SHUTDOWN, which
 * maps to USBD_CANCELLED - the driver reclaims silently, so the
 * oerrors accounting of a real NetBSD timeout is not reproduced (the
 * wd_timeouts counter stands in for it).  Candidates are collected
 * under ipl_save() but killed outside it: usbh_kill_urb may take
 * milliseconds and synthesize completions. */
static void usbd_watchdog_sweep(struct usbd_device *dev) {
	struct usbd_xfer *overdue[8];
	unsigned stuck_ms[8];
	struct usbd_xfer *xfer;
	unsigned now = wlan_port_now_ms();
	unsigned i, n = 0;
	ipl_t ipl;

	ipl = ipl_save();
	SLIST_FOREACH(xfer, &dev->in_flight_xfers, wd_next) {
		if ((xfer->in_flight || xfer->in_xq) &&
		    xfer->wd_deadline != 0U &&
		    (int) (now - xfer->wd_deadline) >= 0) {
			/* clear the deadline so the next sweep cannot
			 * double-kill while the synthesized completion
			 * still sits in the ring */
			stuck_ms[n] = now - xfer->wd_deadline + xfer->timeout;
			xfer->wd_deadline = 0U;
			overdue[n++] = xfer;
			if (n == (sizeof(overdue) / sizeof(overdue[0]))) {
				break;
			}
		}
	}
	ipl_restore(ipl);

	for (i = 0; i < n; i++) {
		if (overdue[i]->in_xq) {
			/* parked xfers never reached the HCD: there is no
			 * urb to kill, hand the timeout straight back */
			wlan_usb_stats.q_timeout++;
			wlan_port_serializer_lock();
			ipl = ipl_save();
			usbd_xq_unlink(overdue[i]->pipe, overdue[i]);
			SLIST_REMOVE(&dev->in_flight_xfers, overdue[i],
			    usbd_xfer, wd_next);
			ipl_restore(ipl);
			overdue[i]->in_xq = 0;
			overdue[i]->status = USBD_TIMEOUT;
			overdue[i]->actlen = 0;
			usbd_ring_post(dev, overdue[i]);
			wlan_port_serializer_unlock();
			continue;
		}
		wlan_usb_stats.kill_calls++;
		wlan_usb_stats.wd_timeouts++;
		printf("[wlan] usbdi watchdog: killing xfer stuck %u ms "
		    "(timeout=%u)\n", stuck_ms[i], overdue[i]->timeout);
		(void) usbh_kill_urb(&overdue[i]->urb);
	}
}

static void *wlan_urb_worker_loop(void *arg) {
	struct usbd_device *dev = arg;

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

			if (xfer == NULL || xfer->magic != USBD_SHIM_MAGIC) {
				/* a corrupted entry must never reach the
				 * driver: count it and move on */
				wlan_usb_stats.guard_drop++;
				continue;
			}
			usbd_shim_urb_work(xfer);
		}

		usbd_watchdog_sweep(dev);
	}
	return NULL;
}

static void *wlan_taskq_worker_loop(void *arg) {
	struct usbd_device *dev = arg;

	while (dev->running) {
		struct usb_task *task;
		ipl_t ipl;

		(void) usb_osal_sem_take(dev->task_sem, USB_OSAL_WAITING_FOREVER);
		ipl = ipl_save();
		if (dev->task_head == 0) {
			ipl_restore(ipl);
			continue;
		}
		task = dev->tasks[0];
		memmove(&dev->tasks[0], &dev->tasks[1],
		    (dev->task_head - 1) * sizeof(task));
		dev->task_head--;
		ipl_restore(ipl);

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
	return NULL;
}

static void wlan_workers_start(struct usbd_device *dev) {
	if (dev->workers_started) {
		return;
	}
	dev->workers_started = 1;
	dev->running = 1;
	dev->ring_sem = usb_osal_sem_create(0);
	dev->task_sem = usb_osal_sem_create(0);
	mutex_init(&dev->wq_mtx, MUTEX_DEFAULT, IPL_USB);
	cv_init(&dev->wq_cv, "wlanurb");

	dev->urb_worker = wlan_port_thread_create(wlan_urb_worker_loop, dev);
	wlan_port_thread_start(dev->urb_worker);
	dev->taskq_worker = wlan_port_thread_create(wlan_taskq_worker_loop, dev);
	wlan_port_thread_start(dev->taskq_worker);
}

/* ------------------------------------------------------------------ */
/* attach: called by the cherryusb class hook when the dongle appears */

int wlan_usbdi_attach(struct wlan_usb_dev *port,
	const struct wlan_chip_driver *drv) {
	struct usbd_device *dev;
	struct usbh_hubport *hport = port->env_dev;

	dev = usbd_shim_register_device(hport, port);
	if (dev == NULL) {
		return -ENOMEM;
	}
	port->port_priv = dev;

	wlan_workers_start(dev);

	return drv->attach(port, NULL);
}

void wlan_usbdi_detach(struct wlan_usb_dev *port) {
	struct usbd_device *dev = port->port_priv;

	if (dev == NULL) {
		return;
	}
	dev->running = 0;
	usb_osal_sem_give(dev->ring_sem);
	usb_osal_sem_give(dev->task_sem);
	port->port_priv = NULL;
	/* the shim stays allocated: in-flight completions and the
	 * driver's xfers reference it, and this harness never
	 * re-attaches without a reboot */
}
