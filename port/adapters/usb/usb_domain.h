/*
 * @file
 * @brief RK3568 USB power/PHY domain bring-up - the part of the platform
 * sequence that is the same whichever host stack drives the controllers.
 *
 * Moved out of port/adapters/libbsd/usb_platform.c (feat/cherryusb_ehci):
 * the register sequence is the register-exact NetBSD port of this board
 * (rk_usb2phy.c + dwc3_fdt.c, netbsd-11) and was proven from a cold USB
 * domain at every NetBSD and freewebcamera cold boot since 2026-09-03.
 * The CherryUSB backend needs exactly the same PD_PIPE + PHY reference
 * clock + VBUS + usb2phy1 sequence before its EHCI registers are touched,
 * and a second copy of these registers is a second place to get the CRU
 * write protocol wrong (high-half write enable; one bit per pin on GRF).
 *
 * Only the shared part lives here: the USB3 socket-group domain and the
 * DWC3 core reconfiguration stay in usb_xhci_platform.c, and the HCD
 * composition (ehci_softc + config_found, or CherryUSB's usbh_initialize)
 * stays with its backend.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#ifndef USB_DOMAIN_H
#define USB_DOMAIN_H

#include <stdint.h>

/* Microsecond busy-wait off the ARM generic timer (always on at EL1). */
void usb_udelay(uint32_t usec);

/* PD_PIPE power island on + PHY reference clock gates + VBUS enable pins.
 * Runs once per boot: re-running a soft reset on a live island takes down
 * already-attached controllers (the rk_usb2phy once-guard lesson). */
void usb_bus_domain_once(void);

/* The usb2phy1 domain for the two panel EHCI roots (both roots share the
 * one PHY domain; the shared bus domain runs first). Runs once per boot. */
void usb_usb2phy1_domain_init(void);

#endif /* USB_DOMAIN_H */
