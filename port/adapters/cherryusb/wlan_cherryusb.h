/*
 * @file
 * @brief Internal seam between the CherryUSB host stack and the usbdi(9)
 *        compat layer (usbdi_compat.c) plus its diagnostics.
 *
 * Only two translation units are meant to see this header: the compat
 * layer itself and the CherryUSB class hook that claims a wireless dongle
 * (usbh_urtwn_class.c).  The hook hands the enumerated hubport to
 * wlan_usbdi_attach(), gets back the real NetBSD struct usbd_device the
 * verbatim driver will be attached against, and later hands the same
 * pointer to wlan_usbdi_detach() - the natural place for it is the
 * CherryUSB interface's own `priv` field
 * (hport->config.intf[intf].priv), which the disconnect hook still has.
 *
 * The driver-facing usbd_* prototypes are NOT here: they are the imported
 * usbdi(9) surface (dev/usb/usbdi.h), which the driver already includes.
 * What lives here is only what the shim adds on top of that surface.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#ifndef FREEWEBCAMERA_WLAN_CHERRYUSB_H_
#define FREEWEBCAMERA_WLAN_CHERRYUSB_H_

#include <stdint.h>

struct usbh_hubport;
struct usbd_device;

/* Build the real usbd_device over an enumerated CherryUSB hubport: copy
 * the device descriptor, build the contiguous NetBSD config-descriptor
 * buffer from CherryUSB's parsed configuration, fill ud_ifaces[] with the
 * per-interface descriptors/endpoints, and start the per-device urb and
 * taskq workers.  drv_ctx is opaque bookkeeping for the caller (the
 * matched chip driver); the shim only stores it.
 *
 * Returns NULL when the port is not connected or carries no usable
 * configuration (the caller must then leave the device unclaimed).
 * Idempotency is not attempted: one call per enumeration, no reboot-free
 * re-attach (same rule the old harness had). */
struct usbd_device *wlan_usbdi_attach(struct usbh_hubport *hport,
    void *drv_ctx);

/* Stop the per-device workers and wake them off their semaphores.  The
 * device object and every descriptor buffer stay allocated on purpose:
 * in-flight completions and the driver's xfers still reference them, and
 * this harness never re-attaches without a reboot. */
void wlan_usbdi_detach(struct usbd_device *dev);

/* Monotonic milliseconds for the shim's xfer-timeout watchdog and its
 * latency accounting.  This port carries its own clock because
 * wlan_port_now_ms() is declared in port.h but has no implementation in
 * port/adapters/libbsd/osal/osal_cmsis_rtos2.c; the source is the CMSIS
 * kernel tick, so the unit is the kernel's (1 ms on this build).  Wrap
 * around is tolerated: deadlines are compared as signed deltas. */
unsigned int wlan_usbdi_now_ms(void);

/* Shim-side diagnostics (`wlan usbstats` style dumps and the budgeted
 * async trace the bring-up rounds use).  All are reset by trace_reset(),
 * which the port core calls right before if_init so the trace starts from
 * zero inside the driver's init sequence. */
void wlan_usbdi_stats_dump(void);
void wlan_usbdi_trace_set(unsigned int level);
void wlan_usbdi_trace_reset(void);

/* The NetBSD speed code this shim reports for a device (USB_SPEED_* of
 * dev/usb/usb.h, not the CherryUSB numbering).  Not part of the imported
 * usbdi(9) surface - the rtw88 line reads it, urtwn does not. */
uint8_t usbd_get_speed(struct usbd_device *dev);

/* RX buffer cache maintenance across the non-coherent USB DMA boundary.
 * The rtw88 line arms its RX buffers with these; harmless for urtwn
 * (whose completions are already invalidated by the EHCI port). */
void usbd_rx_buffer_invalidate(void *buffer, uint32_t length);
void usbd_rx_buffer_arm(void *buffer, uint32_t length);

#endif /* FREEWEBCAMERA_WLAN_CHERRYUSB_H_ */
