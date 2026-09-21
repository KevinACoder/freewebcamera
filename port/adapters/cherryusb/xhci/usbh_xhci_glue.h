/*
 * @file   usbh_xhci_glue.h
 * @brief  Adapter-side view of the xHCI glue: the observability the start
 *         path and the acceptance runs need without reaching into the
 *         transplanted driver's statics.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#ifndef USBH_XHCI_GLUE_H
#define USBH_XHCI_GLUE_H

#include <stdint.h>
#include <stdbool.h>

/* True once the driver's usb_hc_init() has the controller running (the
 * glue's low-level init gates register access, so this is also the "MMIO is
 * safe to touch" flag for the start-path poll). */
bool usbh_xhci_hc_running(uint8_t busid);

/* Root port count from HCSPARAMS1 (also published into the roothub during
 * low-level init, before the hub thread can enumerate). */
uint8_t usbh_xhci_nports(uint8_t busid);

/* Raw PORTSC of a root port (1-based), for the post-init diagnostics. */
uint32_t usbh_xhci_portsc(uint8_t busid, uint8_t port);

/* Monotonic Port Status Change event counter (ISR-incremented in the
 * driver, per instance). The hot-plug watchdog polls it: the vendored
 * roothub_intbuf is never cleared by the stack, so only a change in this
 * sequence tells "new port event" from "already handed to the hub thread". */
uint32_t usbh_xhci_port_evt_seq(uint8_t busid);

/* The xHCI-flavoured low-level hooks. Not the generic usb_hc.h contract
 * names: usbh_glue.c dispatches that contract per busid and calls these for
 * xHCI buses (multi-HCD builds only). */
struct usbh_bus;
void usbh_xhci_low_level_init(struct usbh_bus *bus);
void usbh_xhci_low_level_deinit(struct usbh_bus *bus);

/* The driver's routing table for usbh_hcd_register() (the IRQ trampolines
 * also fire through it). */
extern const struct usbh_hcd_ops usbh_xhci_ops;

#endif /* USBH_XHCI_GLUE_H */
