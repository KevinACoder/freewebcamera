/*
 * @file   usbh_xhci_glue.c
 * @brief  RK3568 xHCI glue for CherryUSB: the xHCI-side platform hooks for
 *         the multi-HCD image (both DWC3 instances: usbhost_dwc3 @
 *         0xFD000000 and otg0-as-host @ 0xFCC00000).
 *
 * The generic hooks (usb_hc_low_level_init/deinit, the dcache plumbing, the
 * aligned allocator's EHCI twin) live in ../usbh_glue.c, which dispatches
 * per busid; this file carries the xHCI-flavoured pieces only. Two names
 * deliberately differ from the generic contract:
 *  - usbh_xhci_low_level_init/deinit: called through the generic hooks'
 *    busid dispatch, never linked against the driver's plain-symbol calls;
 *  - the interrupt is armed late (usb_hc_enable_interrupt) because the
 *    NetBSD-port driver finishes init before it can take a line hit, while
 *    the EHCI arms inside its low-level init.
 *
 * Platform ownership (decision D38, revising D36): the OS runs the whole
 * USB domain bring-up before touching the xHCI registers - usbh_platform.c's
 * usbh_rk3568_usb3otg_domain_init(instance) is the register-exact NetBSD
 * rk_usb2phy.c + dwc3_fdt.c sequence (PD_PIPE ensure, clock gates, the
 * SRST_USB3OTG pulse that wipes whatever U-Boot's preboot `usb start` left
 * behind, VBUS, usb2phy0 GRF, per-instance dwc3 soft reset + PHY quirks +
 * PRTCAP=host). That stack is the only xHCI reference board-proven on this
 * SoC from a cold USB domain (2026-09-03).
 *
 * Register access is gated on s_xhci_regs_alive[instance] (set after the
 * platform sequence - the SRST pulse briefly kills the aperture);
 * usbh_bus_start() polls from task context.
 *
 * @author zhugengyu
 * @date   22.09.2026
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "irq_ctrl.h"

#include "usbh_core.h"
#include "usb_osal.h"
#include "usb_board.h"
#include "usbh_platform.h"
#include "usb_hc_xhci.h"
#include "usbh_xhci_glue.h"

static volatile bool s_xhci_regs_alive[USBH_XHCI_NUM];

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

/* --- register access helpers ---------------------------------------------- */

/* The xHCI register frame sits at the DWC3 base; CAPLENGTH picks the
 * operational-register offset (same controller, one aperture). */
static uintptr_t usbh_xhci_opbase(uint8_t busid)
{
	uint32_t caplen =
		*(volatile uint32_t *)(USBH_XHCI_BASE(busid) + XHCI_CAPLENGTH);

	return USBH_XHCI_BASE(busid) + (caplen & 0xFFU);
}

bool usbh_xhci_hc_running(uint8_t busid)
{
	uint32_t cmd, sts;

	if (!USBH_BUS_IS_XHCI(busid) ||
	    !s_xhci_regs_alive[USBH_XHCI_INST(busid)]) {
		return false;
	}
	/* 在平台序列把 xHCI 环复位出来之前(RUN 已写)轮询就绪; 平台序列期间
	 * SRST 脉冲会让孔径短暂不可读, s_xhci_regs_alive 置位顺序保证读不到
	 * 半初始化状态。 */
	cmd = *(volatile uint32_t *)(usbh_xhci_opbase(busid) + XHCI_USBCMD);
	sts = *(volatile uint32_t *)(usbh_xhci_opbase(busid) + XHCI_USBSTS);
	return ((cmd & XHCI_CMD_RS) != 0U) && ((sts & XHCI_STS_HCH) == 0U);
}

uint8_t usbh_xhci_nports(uint8_t busid)
{
	uint32_t hcs1;

	if (!USBH_BUS_IS_XHCI(busid) ||
	    !s_xhci_regs_alive[USBH_XHCI_INST(busid)]) {
		return 0U;
	}
	/* HCSPARAMS1 lives in the CAPABILITY frame (capbase+0x04), not in the
	 * operational frame: reading it at opbase+0x04 lands on USBSTS, whose
	 * top byte is zero - the roothub got nports=0 and the hub thread's
	 * port loop never ran (root cause of the silent no-enumeration boot
	 * of 2026-09-20). The xHCI register frames share one aperture, so the
	 * cap offset applies from the DWC3 base directly. */
	hcs1 = *(volatile uint32_t *)(USBH_XHCI_BASE(busid) + XHCI_HCSPARAMS1);
	return (uint8_t)XHCI_HCS1_N_PORTS(hcs1);
}

/* Raw PORTSC of a root port (1-based), for the post-init diagnostics. */
uint32_t usbh_xhci_portsc(uint8_t busid, uint8_t port)
{
	if (!USBH_BUS_IS_XHCI(busid) || !s_xhci_regs_alive[USBH_XHCI_INST(busid)] ||
	    port == 0U) {
		return 0U;
	}
	return *(volatile uint32_t *)(usbh_xhci_opbase(busid) + XHCI_PORTSC(port));
}

/* --- interrupts ------------------------------------------------------------ */

/* The CMSIS irq_ctrl handler carries no argument; each xHCI instance gets
 * its own trampoline closing over the busid. The driver's USBH_IRQHandler
 * was renamed usbh_xhci_irq in the multi-HCD build - routing through the
 * ops table's irq member keeps this file free of the renamed symbol. */
static void usbh_xhci0_isr(void)
{
	usbh_xhci_ops.irq(USBH_XHCI0_BUSID);
}

static void usbh_xhci1_isr(void)
{
	usbh_xhci_ops.irq(USBH_XHCI1_BUSID);
}

static void (*const s_xhci_isr[USBH_XHCI_NUM])(void) = {
	usbh_xhci0_isr,
	usbh_xhci1_isr,
};

/* --- usb_hc.h platform hooks (xHCI flavour, dispatched by usbh_glue.c) ----- */

void usbh_xhci_low_level_init(struct usbh_bus *bus)
{
	uint8_t busid = bus->busid;
	uint32_t irq_num;

	if (!USBH_BUS_IS_XHCI(busid)) {
		return;
	}

	/* D38: OS 侧全序平台 bring-up(NetBSD rk_usb2phy + dwc3_fdt 序列;
	 * 域部分 once, dwc3 寄存器段按实例)。跑完之后该实例的 xHCI 孔径才是
	 * 已知的干净状态。 */
	usbh_rk3568_usb3otg_domain_init(USBH_XHCI_INST(busid));

	s_xhci_regs_alive[USBH_XHCI_INST(busid)] = true;
	/* publish the real root port count into the roothub - this driver has
	 * no post-HCRESET hook like the EHCI one, and the hub thread would
	 * otherwise enumerate CONFIG_USBHOST_MAX_RHPORTS phantom ports. */
	bus->hcd.roothub.nports = usbh_xhci_nports(busid);

	/* Handler + priority here, enable only when the driver finishes
	 * init (usb_hc_enable_interrupt): no line storms mid-init. FromISR
	 * calls happen inside, so the line sits at the board's API-call
	 * priority class - same policy as every other driver IRQ. */
	irq_num = USBH_XHCI_IRQ(busid);
	(void)IRQ_SetHandler((IRQn_ID_t)irq_num, s_xhci_isr[USBH_XHCI_INST(busid)]);
	(void)IRQ_SetPriority((IRQn_ID_t)irq_num,
			      BOARD_IRQ_PRIORITY_API_CALL_RAW);
}

void usbh_xhci_low_level_deinit(struct usbh_bus *bus)
{
	if (!USBH_BUS_IS_XHCI(bus->busid)) {
		return;
	}
	(void)IRQ_Disable((IRQn_ID_t)USBH_XHCI_IRQ(bus->busid));
	s_xhci_regs_alive[USBH_XHCI_INST(bus->busid)] = false;
}

/* id is the busid (the driver passes bus->busid straight through). */
unsigned long usb_hc_get_register_base(uint32_t id)
{
	if (!USBH_BUS_IS_XHCI(id) || USBH_XHCI_INST(id) >= USBH_XHCI_NUM) {
		return 0UL;
	}
	return (unsigned long)USBH_XHCI_BASE(id);
}

void usb_hc_enable_interrupt(uint32_t id)
{
	if (USBH_BUS_IS_XHCI(id)) {
		(void)IRQ_Enable((IRQn_ID_t)USBH_XHCI_IRQ(id));
	}
}

void usb_hc_disable_interrupt(uint32_t id)
{
	if (USBH_BUS_IS_XHCI(id)) {
		(void)IRQ_Disable((IRQn_ID_t)USBH_XHCI_IRQ(id));
	}
}
