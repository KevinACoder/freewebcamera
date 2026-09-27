/*
 * @file   usbh_glue.c
 * @brief  RK3568 EHCI glue for CherryUSB: the strong low-level hooks the
 *         vendored usb_hc_ehci.c expects, plus the dcache plumbing and the
 *         console formatter.
 *
 * Shape follows the net_80211 line's usbh_glue.c (board-verified
 * 2026-09-19: dual EHCI through the onboard CH334P hubs enumerating
 * RTL8188EUS / AIC8800D80), with this repo's two differences:
 *  - the shared domain sequence now lives in port/adapters/usb/usb_domain.c
 *    (the NetBSD HCD backend links the same file - one copy of the
 *    register protocol, not two);
 *  - the interrupt dispatch goes through upstream's multi-HC dispatcher
 *    USBH_IRQHandler(busid), so the trampolines carry no driver symbol.
 *
 * The invalidate stays a bare ivac on purpose: it is only ever called on
 * buffers the CPU is about to read after the device wrote them, and a
 * clean-and-invalidate (civac) would write the CPU's stale dirty lines back
 * over the DMA data.
 *
 * @date   27.09.2026
 * @author zhugengyu
 */

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#include "board.h"
#include "irq_ctrl.h"

#include "usbh_core.h"
#include "usb_hc_ehci.h"
#include "usb_board.h"
#include "usb_domain.h"

/* the console formatter the shadow usb_config.h points CONFIG_USB_PRINTF at:
 * one line-buffer under the board print lock (the log macros are reached
 * from the hub threads and from error paths), formatted with minilibc's
 * vsnprintf. Message text ends in '\n'; the sink owns the CRLF and the
 * timestamp (print discipline, AGENTS.md). */
void usbh_console_printf(const char *fmt, ...)
{
	static char buf[192];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	board_early_print(buf);
}

static const uint32_t s_ehci_irq[USBH_EHCI_NUM] = {
	USBH_EHCI0_IRQ,
	USBH_EHCI1_IRQ,
};

static const uint32_t s_xhci_irq[USBH_XHCI_NUM] = {
	USBH_XHCI0_IRQ,
	USBH_XHCI1_IRQ,
};

/* The completion path mutates the same shared state (async ring, pool
 * freelists, urb fields) the submit path guards with the osal critical
 * section, and the submit side may run on a thread the IRQ preempts.
 * Serializing the dispatch closes that window (the net_80211 line measured
 * the tear as double-granted pool slots under full-rate bulk,
 * evidence 20260923-fullrate-instability-two-signatures). Hold time is the
 * scan itself; nothing in the held region sleeps. */
static void s_hcd_isr(uint8_t busid)
{
	size_t flags = usb_osal_enter_critical_section();

	USBH_IRQHandler(busid);

	usb_osal_leave_critical_section(flags);
}

/* The CMSIS irq_ctrl handler carries no argument, so each bus gets its own
 * trampoline closing over the busid. */
static void usbh_ehci0_isr(void)
{
	s_hcd_isr(0U);
}

static void usbh_ehci1_isr(void)
{
	s_hcd_isr(1U);
}

static void usbh_xhci0_isr(void)
{
	s_hcd_isr(USBH_XHCI0_BUSID);
}

static void usbh_xhci1_isr(void)
{
	s_hcd_isr(USBH_XHCI1_BUSID);
}

static void (*const s_ehci_isr_tab[USBH_EHCI_NUM])(void) = {
	usbh_ehci0_isr,
	usbh_ehci1_isr,
};

static void (*const s_xhci_isr_tab[USBH_XHCI_NUM])(void) = {
	usbh_xhci0_isr,
	usbh_xhci1_isr,
};

/* Called from the vendored usb_hc_init() with the registers still reset:
 * bring up the shared PHY domain and arm the interrupt line. The register
 * base itself was set by usbh_initialize() before this runs.  EHCI busids
 * run the usb2phy1 domain; xHCI busids run the USB3 socket-group domain +
 * the DWC3 core reconfiguration (both once-guarded - usbh_platform_start()
 * has already run them in task context, before any hub thread existed,
 * because the con9/con14 SRST pulses reset whole controllers). */
void usb_hc_low_level_init(struct usbh_bus *bus)
{
	uint32_t irq_num;

	if (USBH_BUS_IS_XHCI(bus->busid)) {
		if (USBH_XHCI_INST(bus->busid) >= USBH_XHCI_NUM) {
			return;
		}

		usb_usb3_domain_init();
		usb_xhci_dwc3_host_init(bus->hcd.reg_base);

		irq_num = s_xhci_irq[USBH_XHCI_INST(bus->busid)];
		(void)IRQ_SetHandler((IRQn_ID_t)irq_num,
				     s_xhci_isr_tab[USBH_XHCI_INST(bus->busid)]);
		(void)IRQ_SetPriority((IRQn_ID_t)irq_num,
				      BOARD_IRQ_PRIORITY_API_CALL_RAW);
		(void)IRQ_Enable((IRQn_ID_t)irq_num);
		return;
	}

	if ((uint32_t)bus->busid >= USBH_EHCI_NUM) {
		return;
	}

	usb_usb2phy1_domain_init();

	irq_num = s_ehci_irq[bus->busid];
	(void)IRQ_SetHandler((IRQn_ID_t)irq_num, s_ehci_isr_tab[bus->busid]);
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
