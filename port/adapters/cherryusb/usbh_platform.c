/*
 * @file   usbh_platform.c
 * @brief  RK3568 usb2phy1 domain bring-up for the two panel EHCI controllers.
 *
 * Sequence and constants are the board-verified set from the standalone line
 * (os/freertos usb_rk3568_host_platform.c, itself taken from the NetBSD
 * rk_usb2phy.c measurements on this same board). Only the EHCI-relevant half
 * of that file travelled here: PD_PIPE power, the PHY reference clock gates,
 * VBUS, the USB2HOST soft resets and the usb2phy1 GRF port/clkout controls.
 * The DWC3/xHCI core setup did not - those controllers are not in this image.
 *
 * Ordering rules that keep this safe to call per-bus:
 *  - The PD_PIPE/VBUS/PMUCRU part runs once (idempotent power-on; re-running
 *    a soft reset on a live island takes down already-attached controllers -
 *    the NetBSD rk_usb2phy lesson).
 *  - The usb2phy1 sequence also runs once: both EHCI roots share the one PHY
 *    domain, so the second bus's init must not re-toggle it.
 *
 * Runs in task context (the usb_start() task); the waits here are busy
 * microseconds plus a few 2ms sleeps worth of PHY settle.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#include <stdint.h>
#include <stdbool.h>

#include "usbh_platform.h"
#include "usb_board.h"

static bool s_usb_bus_domain_done;
static bool s_usb2phy1_domain_done;

/* Microsecond busy-wait off the ARM generic timer (always on at EL1). */
static void usb_udelay(uint32_t usec)
{
	uint64_t start, now, ticks, freq;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
	ticks = (freq * (uint64_t)usec) / 1000000U;
	if (ticks == 0U) {
		ticks = 1U;
	}
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(start));
	for (;;) {
		__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(now));
		if (now - start >= ticks) {
			break;
		}
	}
}

/* PD_PIPE power island on + PHY reference clock gates + VBUS enable pins.
 * Bus-island power-up: request idle, wait for the ack, then ungate the
 * domain and wait for the power-down status bit to clear. */
static void usb_bus_domain_once(void)
{
	volatile uint32_t *pmu = (volatile uint32_t *)USBH_PMU_BASE;
	volatile uint32_t *pmucru = (volatile uint32_t *)USBH_PMUCRU_BASE;
	volatile uint32_t *ddr = (volatile uint32_t *)(USBH_GPIO3_BASE +
						       USBH_GPIO3_SWPORT_DDR);
	volatile uint32_t *dr = (volatile uint32_t *)(USBH_GPIO3_BASE +
						      USBH_GPIO3_SWPORT_DR);
	uint32_t i;

	/* PD_PIPE bus idle request release (write-enable only, data 0 - the
	 * standalone-line sequence verbatim), then wait for the ack to clear. */
	*(pmu + USBH_PMU_BUS_IDLE_SFTCON0 / 4U) =
		GRF_WR(USBH_PMU_PD_PIPE_IDLE_BIT, 0U);
	for (i = 0; i < 100U; i++) {
		if ((*(pmu + USBH_PMU_BUS_IDLE_ACK / 4U) &
		     USBH_PMU_PD_PIPE_IDLE_BIT) == 0U) {
			break;
		}
		usb_udelay(100);
	}

	/* Power the domain on and wait for the power-down status to clear. */
	*(pmu + USBH_PMU_PWR_GATE_SFTCON / 4U) =
		GRF_WR(USBH_PMU_PD_PIPE_BIT, 0U);
	for (i = 0; i < 100U; i++) {
		if ((*(pmu + USBH_PMU_PWR_DWN_ST / 4U) &
		     USBH_PMU_PD_PIPE_BIT) == 0U) {
			break;
		}
		usb_udelay(100);
	}

	/* PHY reference clocks (usbphy0 + usbphy1 + ref24m). CRU clkgate
	 * polarity: low-bit 1 = gate OFF, 0 = clock enabled - data 0 turns
	 * the clocks on (the standalone sequence writes (0x3 << 16) here). */
	*(pmucru + USBH_PMUCRU_CLKGATE_CON2 / 4U) =
		GRF_WR(USBH_PMUCRU_USBPHY_GATES, 0U);

	/* VBUS: GPIO3_A0/A1 as output-high (hi-word = direction write, low
	 * word = data write; both set the pin). */
	*ddr = GRF_WR(USBH_VBUS_PINS, USBH_VBUS_PINS);
	*dr = GRF_WR(USBH_VBUS_PINS, USBH_VBUS_PINS);

	s_usb_bus_domain_done = true;
}

void usbh_rk3568_usb2phy1_domain_init(void)
{
	volatile uint32_t *cru = (volatile uint32_t *)USBH_CRU_BASE;
	volatile uint32_t *pmucru = (volatile uint32_t *)USBH_PMUCRU_BASE;
	volatile uint32_t *grf =
		(volatile uint32_t *)USBH_USB2PHY1_GRF_BASE;

	if (s_usb2phy1_domain_done) {
		return;
	}

	if (!s_usb_bus_domain_done) {
		usb_bus_domain_once();
	}

	/* PMUCRU: add the usb2phy1 reference bit on top of what the bus
	 * domain already ungated (clkgate data 0 = clock enabled). */
	*(pmucru + USBH_PMUCRU_CLKGATE_CON2 / 4U) =
		GRF_WR(USBH_PMUCRU_USBPHY_GATES, 0U);

	/* USB2HOST0/1 + their ARB/UTMI interfaces: assert, settle, release. */
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS, USBH_CRU_SOFTRST_CON14_BITS);
	usb_udelay(100);
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS, 0U);
	usb_udelay(100);

	/* PHY1 POR/port reset and the GRF pclk: release only, never assert -
	 * asserting the POR here would drop a PHY the other bus is using. */
	*(cru + USBH_CRU_SOFTRST_CON29 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON29_BITS, 0U);
	usb_udelay(100);
	*(cru + USBH_CRU_SOFTRST_CON28 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON28_BITS, 0U);
	usb_udelay(100);

	/* Port GRF: otg-port then host-port suspend release, then open the
	 * 480m UTMI clock output. Each step gets its settle time. */
	*(grf + USBH_USB2PHY_GRF_OTG_CON0 / 4U) =
		GRF_WR(USBH_USB2PHY_OTG_SUS_MASK, USBH_USB2PHY_OTG_SUS_VAL);
	usb_udelay(2000);
	*(grf + USBH_USB2PHY_GRF_HOST_CON1 / 4U) =
		GRF_WR(USBH_USB2PHY_HOST_SUS_MASK, USBH_USB2PHY_HOST_SUS_VAL);
	usb_udelay(2000);
	*(grf + USBH_USB2PHY_GRF_CLKOUT_CON2 / 4U) =
		GRF_WR(USBH_USB2PHY_CLKOUT_WE, 0U);
	usb_udelay(2000);

	s_usb2phy1_domain_done = true;
}
