/*
 * @file
 * @brief USB host abstraction backend over the imported NetBSD usbdi stack.
 *
 * Implements include/usb_host.h on top of the usbdi world the platform code
 * brings up (port/adapters/libbsd/usb_platform.c).  This file is the seam's
 * whole point: callers above get device enumeration and synchronous control
 * transfers without including a single usbdi header, so replacing this file
 * with a second backend does not touch them.
 *
 * WHAT THIS BACKEND KNOWS THAT THE INTERFACE DOES NOT
 *
 *  - The bus registry.  usb_platform_bus_at() returns the struct usbd_bus
 *    objects in attach order; devices live in bus->ub_devices[] indexed by
 *    usb_addr2dindex() (root hub at index 1, address N at 1+N).  The
 *    interface's flat "device index" is this backend's invention: it walks
 *    the registry in order and numbers the populated slots.
 *
 *  - The transfer path.  usbd_do_request_len() is the imported stack's
 *    synchronous control-transfer primitive: it creates an xfer on pipe 0,
 *    runs it with a timeout, and destroys it.  The buffer it hands the
 *    controller is allocated inside the xfer (usbd_alloc_buffer, DMA pool),
 *    and usbdi.c copies between it and the caller's buffer on both
 *    directions (see usbd_transfer/usb_transfer_complete) - so this file
 *    passes the caller's pointer straight through and owns no staging.
 *
 *  - The error mapping.  The interface distinguishes STALL / NAK / IO /
 *    SHORT because those are the conditions that actually occur on this
 *    board's bring-up paths; "it returned nonzero" is not actionable.
 *
 *  - The lock.  A control transfer takes the bus mutex, so the call must not
 *    be made from a context that already holds it - task context only, same
 *    rule as the interface states.  The shell command is the caller in tree.
 *
 * @date 28.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <string.h>

/* the same prologue the platform files carry: the whole usbdi world first */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/bus.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbdivar.h>

#include "usb_host.h"
#include "usb_platform.h"

/* ------------------------------------------------------------------
 * enumeration
 */

/* Device slots are numbered across the bus registry: bus 0's table first,
 * then bus 1's.  The mapping from a flat index to (bus, table slot) is done
 * on the fly in the two callers below; both walk in the same order, so the
 * numbering a caller sees from GetDeviceCount/GetDeviceInfo is stable as long
 * as no device is attached or removed between the calls (the interface
 * documents the snapshot semantics). */
static struct usbd_device *usb_host_dev_at(uint32_t index, uint32_t *out_slot)
{
	unsigned int busn;
	uint32_t seen = 0;

	for (busn = 0; busn < usb_platform_bus_count(); busn++) {
		struct usbd_bus *bus = usb_platform_bus_at(busn);
		int slot;

		if (bus == NULL) {
			continue;
		}
		for (slot = 0; slot < USB_TOTAL_DEVICES; slot++) {
			struct usbd_device *dev = bus->ub_devices[slot];

			if (dev == NULL) {
				continue;
			}
			if (seen == index) {
				if (out_slot != NULL) {
					*out_slot = (uint32_t) slot;
				}
				return dev;
			}
			seen++;
		}
	}
	return NULL;
}

static int32_t
usb_host_netbsd_GetDeviceCount(uint32_t *count)
{
	unsigned int busn;
	uint32_t seen = 0;

	if (count == NULL) {
		return USB_HOST_ERROR_PARAMETER;
	}
	for (busn = 0; busn < usb_platform_bus_count(); busn++) {
		struct usbd_bus *bus = usb_platform_bus_at(busn);
		int slot;

		if (bus == NULL) {
			continue;
		}
		for (slot = 0; slot < USB_TOTAL_DEVICES; slot++) {
			if (bus->ub_devices[slot] != NULL) {
				seen++;
			}
		}
	}
	*count = seen;
	return USB_HOST_OK;
}

/* The imported stack's speed codes are 1..5 (USB_SPEED_*); the interface
 * re-numbers them from 0.  Keeping them separate on purpose: the caller's
 * switch should not depend on a vendored header's constants. */
static uint8_t usb_host_speed_map(uint8_t ud_speed)
{
	switch (ud_speed) {
	case USB_SPEED_LOW:
		return USB_HOST_SPEED_LOW;
	case USB_SPEED_FULL:
		return USB_HOST_SPEED_FULL;
	case USB_SPEED_HIGH:
		return USB_HOST_SPEED_HIGH;
	case USB_SPEED_SUPER:
	case USB_SPEED_SUPER_PLUS:
		return USB_HOST_SPEED_SUPER;
	default:
		return USB_HOST_SPEED_FULL;
	}
}

static int32_t
usb_host_netbsd_GetDeviceInfo(uint32_t index, USB_HOST_DEVICE *info)
{
	struct usbd_device *dev;
	usb_device_descriptor_t *dd;

	if (info == NULL) {
		return USB_HOST_ERROR_PARAMETER;
	}
	dev = usb_host_dev_at(index, NULL);
	if (dev == NULL) {
		return USB_HOST_ERROR_PARAMETER;
	}

	dd = &dev->ud_ddesc;
	memset(info, 0, sizeof(*info));
	info->index = (uint8_t) index;
	info->address = dev->ud_addr;
	info->speed = usb_host_speed_map(dev->ud_speed);
	info->device_class = dd->bDeviceClass;
	info->vendor = UGETW(dd->idVendor);
	info->product = UGETW(dd->idProduct);
	info->bcd_device = UGETW(dd->bcdDevice);
	info->num_configurations = dd->bNumConfigurations;
	info->configured = (dev->ud_config != USB_UNCONFIG_NO);
	return USB_HOST_OK;
}

/* ------------------------------------------------------------------
 * control transfers
 */

static int32_t usb_host_status_map(usbd_status status)
{
	switch (status) {
	case USBD_NORMAL_COMPLETION:
		return USB_HOST_OK;
	case USBD_SHORT_XFER:
		return USB_HOST_ERROR_SHORT;
	case USBD_STALLED:
		return USB_HOST_ERROR_STALL;
	case USBD_TIMEOUT:
		return USB_HOST_ERROR_TIMEOUT;
	case USBD_CANCELLED:
	case USBD_INTERRUPTED:
		return USB_HOST_ERROR_TIMEOUT;
	case USBD_INVAL:
	case USBD_BAD_ADDRESS:
	case USBD_NO_ADDR:
	case USBD_NOT_CONFIGURED:
		return USB_HOST_ERROR_PARAMETER;
	case USBD_NOMEM:
		return USB_HOST_ERROR;
	default:
		/* IOERROR and the HCD-internal statuses land here; the bus
		 * reported something the USB spec has no name for. */
		return USB_HOST_ERROR_IO;
	}
}

static int32_t
usb_host_netbsd_ControlTransfer(uint32_t index, const USB_HOST_REQUEST *req,
    void *data, uint32_t *actlen, uint32_t timeout_ms)
{
	struct usbd_device *dev;
	usb_device_request_t ureq;
	usbd_status status;
	int got = 0;
	size_t len;

	if (req == NULL || (req->wLength != 0 && data == NULL)) {
		return USB_HOST_ERROR_PARAMETER;
	}
	if (actlen != NULL) {
		*actlen = 0;
	}
	dev = usb_host_dev_at(index, NULL);
	if (dev == NULL) {
		return USB_HOST_ERROR_PARAMETER;
	}

	/* The interface's field set IS the setup packet; this is the only
	 * translation, and it is field-for-field. */
	ureq.bmRequestType = req->bmRequestType;
	ureq.bRequest = req->bRequest;
	USETW(ureq.wValue, req->wValue);
	USETW(ureq.wIndex, req->wIndex);
	USETW(ureq.wLength, req->wLength);

	len = req->wLength;
	/* 0 = the interface's "use the stack default"; usbdi's own default is
	 * USBD_DEFAULT_TIMEOUT (5 s), which is what a zero timeout would be
	 * asked for on a device that is not answering. */
	status = usbd_do_request_len(dev, &ureq, len, data, 0, &got,
	    timeout_ms != 0 ? timeout_ms : USBD_DEFAULT_TIMEOUT);

	if (actlen != NULL) {
		*actlen = (got > 0) ? (uint32_t) got : 0;
	}
	return usb_host_status_map(status);
}

/* ------------------------------------------------------------------
 * controller-level
 */

static int32_t usb_host_netbsd_Initialize(USB_HOST_SignalEvent_t cb_event)
{
	/* the buses are attached by usb_platform_init() before anything can
	 * be enumerated; there is nothing left for Initialize to do but say
	 * whether that happened.  No backend owns the platform bring-up - it
	 * belongs to the platform file. */
	(void) cb_event;
	if (usb_platform_bus_count() == 0) {
		return ARM_DRIVER_ERROR;
	}
	return ARM_DRIVER_OK;
}

static int32_t usb_host_netbsd_Uninitialize(void)
{
	/* teardown is the platform's business (and this image never detaches
	 * a controller); refuse rather than pretend. */
	return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static int32_t usb_host_netbsd_PowerControl(uint32_t index, uint32_t state)
{
	/* VBUS is a fixed GPIO on this board (usb_board.h); the socket group is
	 * always powered once the platform bring-up ran. Accept the call so a
	 * portable caller's power sequence does not have to special-case the
	 * board, but report the truth for an out-of-range index. */
	(void) state;
	if (usb_host_dev_at(index, NULL) == NULL) {
		return USB_HOST_ERROR_PARAMETER;
	}
	return ARM_DRIVER_OK;
}

static USB_HOST_CAPABILITIES usb_host_netbsd_GetCapabilities(void)
{
	USB_HOST_CAPABILITIES caps;

	memset(&caps, 0, sizeof(caps));
	caps.max_devices = USB_HOST_MAX_DEVICES;
	caps.high_speed = 1;	/* EHCI roots + xHCI's USB2 bus */
	caps.super_speed = 1;	/* the xHCI instance when present */
	caps.isochronous = 0;	/* not in this interface; see usb_host.h */
	return caps;
}

static ARM_DRIVER_VERSION usb_host_netbsd_GetVersion(void)
{
	ARM_DRIVER_VERSION v = { .api = USB_HOST_API_VERSION, .drv = 0x0100 };

	return v;
}

ARM_DRIVER_USB_HOST Driver_USB_HOST_NetBSD = {
	.GetVersion = usb_host_netbsd_GetVersion,
	.GetCapabilities = usb_host_netbsd_GetCapabilities,
	.Initialize = usb_host_netbsd_Initialize,
	.Uninitialize = usb_host_netbsd_Uninitialize,
	.PowerControl = usb_host_netbsd_PowerControl,
	.GetDeviceCount = usb_host_netbsd_GetDeviceCount,
	.GetDeviceInfo = usb_host_netbsd_GetDeviceInfo,
	.ControlTransfer = usb_host_netbsd_ControlTransfer,
};
