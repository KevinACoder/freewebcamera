/*
 * @file   usbh_glue.c
 * @brief  RK3568 EHCI glue for CherryUSB: the strong low-level hooks the
 *         vendored usb_hc_ehci.c expects, plus the dcache plumbing.
 *
 * Shape follows the standalone line's usb_glue_rk3568.c (board-verified
 * 2026-09-05), with two changes this repo forces:
 *  - interrupt installation goes through the CMSIS irq_ctrl API instead of
 *    the SDK's finterrupt (handler signature here is void(void), hence the
 *    per-bus trampolines);
 *  - dcache hooks call the board primitives instead of the SDK's cache
 *    wrappers.
 *
 * The invalidate stays a bare ivac on purpose: it is only ever called on
 * buffers the CPU is about to read after the device wrote them, and a
 * clean-and-invalidate (civac) would write the CPU's stale dirty lines back
 * over the DMA data.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "irq_ctrl.h"

#include "usbh_core.h"
#include "usb_hc_ehci.h"
#include "usbh_platform.h"
#include "usb_board.h"

static const uint32_t s_ehci_irq[USBH_EHCI_NUM] = {
	USBH_EHCI0_IRQ,
	USBH_EHCI1_IRQ,
};

/* The CMSIS irq_ctrl handler carries no argument, so each bus gets its own
 * trampoline closing over the busid. */
static void usbh_ehci0_isr(void)
{
	USBH_IRQHandler(0U);
}

static void usbh_ehci1_isr(void)
{
	USBH_IRQHandler(1U);
}

static void (*const s_ehci_isr[USBH_EHCI_NUM])(void) = {
	usbh_ehci0_isr,
	usbh_ehci1_isr,
};

/* Called from the vendored usb_hc_init() with the registers still reset:
 * bring up the shared PHY domain and arm the interrupt line. The register
 * base itself was set by usbh_initialize() before this runs. */
void usb_hc_low_level_init(struct usbh_bus *bus)
{
	uint32_t irq_num;

	if ((uint32_t)bus->busid >= USBH_EHCI_NUM) {
		return;
	}

	usbh_rk3568_usb2phy1_domain_init();

	irq_num = s_ehci_irq[bus->busid];
	(void)IRQ_SetHandler((IRQn_ID_t)irq_num, s_ehci_isr[bus->busid]);
	/* FromISR-class calls happen inside, so the line sits at the board's
	 * API-call priority class - same policy as every other driver IRQ. */
	(void)IRQ_SetPriority((IRQn_ID_t)irq_num,
			      BOARD_IRQ_PRIORITY_API_CALL_RAW);
	(void)IRQ_Enable((IRQn_ID_t)irq_num);
}

/* Called after HCRESET, before the schedules start: publish the real root
 * port count (from HCSPARAMS) into the roothub, which usbh_hub_initialize
 * preset to CONFIG_USBHOST_MAX_RHPORTS. The hub thread enumerates exactly
 * the ports this reports. */
void usb_hc_low_level2_init(struct usbh_bus *bus)
{
	bus->hcd.roothub.nports = g_ehci_hcd[bus->hcd.hcd_id].n_ports;
}

/* Root port speed decode from PORTSC line state; required by the vendored
 * roothub control path. */
uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
	uint32_t regval = EHCI_HCOR->portsc[port - 1U];

	if ((regval & EHCI_PORTSC_LSTATUS_MASK) == EHCI_PORTSC_LSTATUS_KSTATE) {
		return USB_SPEED_LOW;
	}
	if (regval & EHCI_PORTSC_PE) {
		return USB_SPEED_HIGH;
	}
	return USB_SPEED_FULL;
}

/* --- dcache hooks (compiled in by CONFIG_USB_DCACHE_ENABLE) ---------------- */

void usb_dcache_clean(uintptr_t addr, size_t size)
{
	board_dcache_flush(addr, size);
}

void usb_dcache_invalidate(uintptr_t addr, size_t size)
{
	board_dcache_invalidate(addr, size);
}

void usb_dcache_flush(uintptr_t addr, size_t size)
{
	board_dcache_flush(addr, size);
}
