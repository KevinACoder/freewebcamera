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
#ifdef CONFIG_USBHOST_MULTI_HCD
#include "xhci/usbh_xhci_glue.h"
#endif

static const uint32_t s_ehci_irq[USBH_EHCI_NUM] = {
	USBH_EHCI0_IRQ,
	USBH_EHCI1_IRQ,
};

/* In the multi-HCD build the drivers' USBH_IRQHandler symbols are renamed
 * (usbh_ehci_irq/usbh_xhci_irq) and the ops tables carry the irq pointers;
 * route through the table so this file never needs the renamed names. */
#ifdef CONFIG_USBHOST_MULTI_HCD
#define EHCI_IRQ_DISPATCH(busid)	usbh_ehci_ops.irq(busid)
#else
#define EHCI_IRQ_DISPATCH(busid)	USBH_IRQHandler(busid)
#endif

/* The EHCI completion scan mutates the same shared state (async ring,
 * pool freelists, urb fields) the submit path guards with the osal
 * critical section (which on this port is _tx_thread_smp_protect - the
 * thread side is covered).  The IRQ side never took it: on the 4-core
 * build the interrupt lands on core 0 while submits run on other
 * cores, so scan/unlink/waitup ran fully exposed to an in-flight arm.
 * Under full-rate bulk that window tore the async ring and double-granted
 * pool slots (evidence 20260923-fullrate-instability-two-signatures.md).
 * Serializing the dispatch closes it; hold time is the scan itself
 * (us-scale - completions only post to the shim's SPSC ring here), and
 * nothing in the held region sleeps (audited 20260923). */
static void usbh_ehci_irq_cs(uint8_t busid)
{
	size_t flags = usb_osal_enter_critical_section();

	EHCI_IRQ_DISPATCH(busid);

	usb_osal_leave_critical_section(flags);
}

/* The CMSIS irq_ctrl handler carries no argument, so each bus gets its own
 * trampoline closing over the busid. */
static void usbh_ehci0_isr(void)
{
	usbh_ehci_irq_cs(0U);
}

static void usbh_ehci1_isr(void)
{
	usbh_ehci_irq_cs(1U);
}

static void (*const s_ehci_isr[USBH_EHCI_NUM])(void) = {
	usbh_ehci0_isr,
	usbh_ehci1_isr,
};

/* Called from the vendored usb_hc_init() with the registers still reset:
 * bring up the shared PHY domain and arm the interrupt line. The register
 * base itself was set by usbh_initialize() before this runs. Both driver
 * ports call this one symbol, so in the multi-HCD build it dispatches by
 * busid - the EHCI branch below, or the xHCI glue's hooks. */
void usb_hc_low_level_init(struct usbh_bus *bus)
{
	uint32_t irq_num;

#ifdef CONFIG_USBHOST_MULTI_HCD
	if (USBH_BUS_IS_XHCI(bus->busid)) {
		usbh_xhci_low_level_init(bus);
		return;
	}
#endif

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

#ifdef CONFIG_USBHOST_MULTI_HCD
/* Multi-HCD dispatch twin: the xHCI glue needs a deinit of its own (IRQ
 * down, alive-flag off), the EHCI side stays the vendored weak no-op. */
void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
	if (USBH_BUS_IS_XHCI(bus->busid)) {
		usbh_xhci_low_level_deinit(bus);
	}
}
#endif

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
