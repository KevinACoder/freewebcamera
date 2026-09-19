/*
 * @file   usbh_xhci_glue.c
 * @brief  RK3568 xHCI (DWC3 usbhost_dwc3 @ 0xFD000000) glue for CherryUSB:
 *         the strong low-level hooks the transplanted usb_hc_xhci driver
 *         expects, the aligned allocator the driver's rings/contexts live
 *         on, and the interrupt install. Nothing else.
 *
 * Firmware/OS division (the M3-A SATA precedent, applied to USB by
 * decision D36): the PHY, CRU/PMUCRU gates, PD_PIPE power island and VBUS
 * enables belong to U-Boot, whose preboot runs `usb start` on every cold
 * boot. The OS inherits a live controller - measured 2026-09-19: at image
 * entry the xHCI aperture reads 0x01100020 and GHWPARAMS1 0x0160c93b with
 * zero platform writes of our own - and only initializes the controller
 * (xHCI-level HCRST + rings). The lab glue's own CRU/PHY sequence was
 * removed after it measurably killed the controller (config registers
 * answered reset defaults, xHCI aperture and param block read 0).
 *
 * Register access is gated on s_xhci_regs_alive (set at entry - the
 * aperture is live before us); usbh_bus_start() polls from task context.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "irq_ctrl.h"

#include "usbh_core.h"
#include "usb_osal.h"
#include "usb_board.h"
#include "usb_hc_xhci.h"

static volatile bool s_xhci_regs_alive;

/* --- allocator ------------------------------------------------------------ */

/* The driver allocates rings/contexts through usb_sys_malloc_align (the lab
 * tree's pool API). This image has no pool: over-allocate from the kernel
 * heap, align inside, and keep the raw pointer one word below the aligned
 * address so a later free stays possible. The driver memsets the full
 * requested size and the 64B alignment matches CONFIG_USB_ALIGN_SIZE, so no
 * allocation shares a cache line with unrelated data. */
void *usb_sys_malloc_align(size_t align, size_t size)
{
	uintptr_t raw, aligned;

	if (align == 0U) {
		align = CONFIG_USB_ALIGN_SIZE;
	}
	raw = (uintptr_t)usb_osal_malloc(size + align + sizeof(uintptr_t));
	if (raw == 0U) {
		return NULL;
	}
	aligned = (raw + sizeof(uintptr_t) + align - 1U) &
		  ~((uintptr_t)align - 1U);
	((uintptr_t *)aligned)[-1] = raw;
	return (void *)aligned;
}

/* Paired with the above for completeness; the driver never frees (deinit is
 * image teardown, not a runtime path). */
void usb_sys_mem_free(void *ptr)
{
	if (ptr != NULL) {
		usb_osal_free((void *)((uintptr_t *)ptr)[-1]);
	}
}

/* --- dcache hooks (compiled in by CONFIG_USB_DCACHE_ENABLE) ---------------- */

/* Same plumbing as usbh_glue.c (which is not in this image shape). The
 * invalidate stays a bare ivac on purpose: it is only ever called on
 * buffers the CPU is about to read after the device wrote them, and a
 * clean-and-invalidate (civac) would write the CPU's stale dirty lines back
 * over the DMA data. */

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

/* --- register access helpers ---------------------------------------------- */

/* The xHCI register frame sits at the DWC3 base; CAPLENGTH picks the
 * operational-register offset (same controller, one aperture). */
static uintptr_t usbh_xhci_opbase(void)
{
	uint32_t caplen =
		*(volatile uint32_t *)(USBH_XHCI0_BASE + XHCI_CAPLENGTH);

	return USBH_XHCI0_BASE + (caplen & 0xFFU);
}

bool usbh_xhci_hc_running(uint8_t busid)
{
	uint32_t cmd, sts;

	(void)busid;
	if (!s_xhci_regs_alive) {
		return false;
	}
	cmd = *(volatile uint32_t *)(usbh_xhci_opbase() + XHCI_USBCMD);
	sts = *(volatile uint32_t *)(usbh_xhci_opbase() + XHCI_USBSTS);
	return ((cmd & XHCI_CMD_RS) != 0U) && ((sts & XHCI_STS_HCH) == 0U);
}

uint8_t usbh_xhci_nports(uint8_t busid)
{
	uint32_t hcs1;

	(void)busid;
	if (!s_xhci_regs_alive) {
		return 0U;
	}
	/* HCSPARAMS1 lives in the CAPABILITY frame (capbase+0x04), not in the
	 * operational frame: reading it at opbase+0x04 lands on USBSTS, whose
	 * top byte is zero - the roothub got nports=0 and the hub thread's
	 * port loop never ran (root cause of the silent no-enumeration boot
	 * of 2026-09-20). The xHCI register frames share one aperture, so the
	 * cap offset applies from the DWC3 base directly. */
	hcs1 = *(volatile uint32_t *)(USBH_XHCI0_BASE + XHCI_HCSPARAMS1);
	return (uint8_t)XHCI_HCS1_N_PORTS(hcs1);
}

/* Monotonic count of Port Status Change events the ISR has swallowed. The
 * hot-plug watchdog in usbh_adapter compares it between polls: the vendored
 * roothub_intbuf is never cleared by the stack, so buffer contents cannot
 * distinguish "already handled" from "new event" - the sequence can.
 * g_xhci_port_evt_seq is defined (and ISR-incremented) in the driver. */
extern volatile uint32_t g_xhci_port_evt_seq;

uint32_t usbh_xhci_port_evt_seq(uint8_t busid)
{
	(void)busid;
	return g_xhci_port_evt_seq;
}

uint32_t usbh_xhci_portsc(uint8_t busid, uint8_t port)
{
	(void)busid;
	if (!s_xhci_regs_alive || port == 0U) {
		return 0U;
	}
	return *(volatile uint32_t *)(usbh_xhci_opbase() + XHCI_PORTSC(port));
}

/* --- interrupts ------------------------------------------------------------ */

/* The CMSIS irq_ctrl handler carries no argument; the single xHCI bus gets
 * a fixed trampoline. */
static void usbh_xhci0_isr(void)
{
	USBH_IRQHandler(0U);
}

/* --- usb_hc.h platform hooks ----------------------------------------------- */

void usb_hc_low_level_init(struct usbh_bus *bus)
{
	/* The aperture is live at image entry (U-Boot preboot ran usb start
	 * on every cold boot): publish the real root port count into the
	 * roothub - this driver has no post-HCRESET hook like the EHCI one,
	 * and the hub thread would otherwise enumerate
	 * CONFIG_USBHOST_MAX_RHPORTS phantom ports. */
	s_xhci_regs_alive = true;
	bus->hcd.roothub.nports = usbh_xhci_nports(bus->busid);

	/* Handler + priority here, enable only when the driver finishes
	 * init (usb_hc_enable_interrupt): no line storms mid-init. FromISR
	 * calls happen inside, so the line sits at the board's API-call
	 * priority class - same policy as every other driver IRQ. */
	(void)IRQ_SetHandler((IRQn_ID_t)USBH_XHCI0_IRQ, usbh_xhci0_isr);
	(void)IRQ_SetPriority((IRQn_ID_t)USBH_XHCI0_IRQ,
			      BOARD_IRQ_PRIORITY_API_CALL_RAW);
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
	(void)bus;
	(void)IRQ_Disable((IRQn_ID_t)USBH_XHCI0_IRQ);
	s_xhci_regs_alive = false;
}

unsigned long usb_hc_get_register_base(uint32_t id)
{
	if (id >= USBH_XHCI_NUM) {
		return 0UL;
	}
	return USBH_XHCI0_BASE;
}

void usb_hc_enable_interrupt(uint32_t id)
{
	(void)id;
	(void)IRQ_Enable((IRQn_ID_t)USBH_XHCI0_IRQ);
}

void usb_hc_disable_interrupt(uint32_t id)
{
	(void)id;
	(void)IRQ_Disable((IRQn_ID_t)USBH_XHCI0_IRQ);
}
