/*
 * @file   usb.h
 * @brief  USB host interface.
 *
 * The one thing the application layer needs from the USB host: start it.
 * What "it" is - CherryUSB over the two panel EHCI controllers today -
 * belongs to the adapter (port/adapters/cherryusb/), and swapping the stack
 * changes that directory and this function's implementation, not its
 * callers.
 *
 * Enumeration status does not appear here on purpose: devices are visible
 * through the shell (`usbh list -t`), which is the acceptance view this
 * milestone is judged on. A richer interface would freeze decisions that
 * are still moving.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#ifndef FREEWEBCAMERA_USB_H
#define FREEWEBCAMERA_USB_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the USB host: run the usb2phy1 domain sequence, initialize both
 * EHCI controllers and let the stack's hub threads enumerate whatever is
 * plugged in (each panel root port carries an onboard CH334P hub; the two
 * wireless NICs hang behind one of them).
 *
 * Returns 0 when both controllers came up. A bus that fails costs the rest
 * of the bring-up nothing in this signature: the other bus still starts,
 * and `usbh list` says what the stack sees.
 *
 * Context: must be called from a thread, with the scheduler running. It
 * creates the stack's threads, waits for controller init (hundreds of
 * milliseconds at most) and runs the pre-plugged-device kick - none of
 * which can happen from the boot path.
 *
 * Idempotent: the second call returns 0 without doing anything.
 */
int usb_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_USB_H */
