/*
 * @file
 * @brief The usb platform bring-up entry (wlan_adapter calls it after
 * the osal is up).
 */

#ifndef NET80211_USB_PLATFORM_H_
#define NET80211_USB_PLATFORM_H_

#include <stdint.h>

/* Bring up the usb2phy1 domain + the panel EHCI root and start the
 * NetBSD usbus chain on it. Returns 0 on success. */
int usb_platform_init(void);
void usb_platform_dump(void);
void usb_platform_reg_dump(void);
void usb_platform_qh_dump(void);

/* the shared bus-domain helper (PD_PIPE + PHY reference clocks +
 * VBUS): the USB3 domain sequence in usb_xhci_platform.c rides on it */
void usb_bus_domain_once(void);

/* the USB3 socket-group domain (CRU gates + SRST pulse + usb2phy0
 * GRF) and the xHCI host on the upper port, fcc00000 - the
 * dwc3_fdt.c role.  Both domains must run before any HCD attaches
 * (D50: the SRST pulses reset shared USB blocks). */
void usb_usb3_domain_init(void);
int usb_xhci_attach(void);
void usb_xhci_dump(void);
/* raw capability/operational/runtime/doorbell register rows */
void usb_xhci_reg_dump(void);

/* the usb history ring: every state transition the imported core
 * logged, oldest first (max = 0 prints the whole ring) */
void usb_platform_hist_dump(unsigned int max);
/* requested against measured wait times for delay()/usb_delay_ms() */
void usb_platform_delay_test(void);

/* The usbus registry: the bus objects the platform handed to the usb driver,
 * in attach order (each EHCI root first, then the xHCI's two buses).
 * Recorded here because this file is what composes and attaches them; a
 * consumer above the imported world (the usb host abstraction's backend)
 * asks this for "bus N" instead of reaching into the driver's device table. */
struct usbd_bus;
unsigned int usb_platform_bus_count(void);
struct usbd_bus *usb_platform_bus_at(unsigned int index);
/* Attach-time registration; called by usb_platform.c and
 * usb_xhci_platform.c for every bus they hand to the usb driver. */
void usb_platform_bus_record(struct usbd_bus *bus);

#endif /* NET80211_USB_PLATFORM_H_ */
