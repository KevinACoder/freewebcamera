/*
 * @file   usbh_platform.c
 * @brief  RK3568 USB platform bring-up: the usb2phy1 domain for the two
 *         panel EHCI controllers, and the USB3OTG domain for the DWC3/xHCI
 *         USB3 socket group.
 *
 * Both sequences are the register-exact NetBSD port of this board
 * (os/netbsd src sys/arch/arm/rockchip/rk_usb2phy.c + sys/dev/fdt/
 * dwc3_fdt.c, netbsd-11): the same values were measured working on this
 * SoC from a cold USB domain on 2026-09-03 (both USB3 Type-A sockets
 * enumerating, HS bulk saturating a U盘) and have been re-verified at
 * every NetBSD cold boot since.  The EHCI half originally travelled via
 * the standalone line's usb_rk3568_host_platform.c.
 *
 * Ordering rules that keep this safe to call per-bus:
 *  - The PD_PIPE/VBUS/PMUCRU part runs once (idempotent power-on; re-running
 *    a soft reset on a live island takes down already-attached controllers -
 *    the NetBSD rk_usb2phy once-guard lesson).
 *  - Each controller family's sequence also runs once per boot: both EHCI
 *    roots share the one usb2phy1 domain, and the xHCI image owns the
 *    USB3OTG domain wholesale.
 *  - The USB3OTG pulse (SRST_USB3OTG0/1) intentionally wipes whatever
 *    U-Boot's preboot `usb start` left behind - the whole point of D38 is
 *    that the OS boots into a known controller state instead of inheriting
 *    U-Boot's (D36 inherit-only could not get past Address Device).
 *
 * Runs in task context (the usb_start() task); the waits here are busy
 * microseconds plus PHY settle sleeps.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#include <stdint.h>
#include <stdbool.h>

#include "usbh_platform.h"
#include "usb_board.h"
#include "usb_hc_xhci.h"
#include "usb_config.h"
#include "usb_osal.h"

static bool s_usb_bus_domain_done;
static bool s_usb2phy1_domain_done;
static bool s_usb3otg_domain_done;

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

/*
 * The USB3 socket group (usb2phy0 @ 0xFDCA0000): NetBSD rk_usb2phy.c
 * rk_usb2phy_domain_init + the usb2phy0 port writes, then dwc3_fdt.c's
 * soft_reset / enable_phy / set_mode on the 0xFD000000 core. Register
 * values are 1:1 (the board dts quirks: dis_enblslpm, dis-u2-freeclk,
 * dis_u2_susphy, dis_u3_susphy, dis-del-phy-power-chg, dis-rxdet-inp3;
 * phy_type utmi_wide; maximum-speed high-speed; GSNPSID rev 0x300a).
 *
 * NetBSD runs the domain part at usb2phy attach and the dwc3 part at the
 * wrapper attach, in this same order: PD/gates -> SRST pulse -> VBUS ->
 * phy GRF -> dwc3 soft reset -> PHY/quirk regs -> PRTCAP. Here the shared
 * bus-domain helper (PD_PIPE + pmucru gates + VBUS) runs first, then the
 * USB3OTG-specific gates and pulses - same net effect, and VBUS asserted
 * early is what U-Boot does too.
 */
void usbh_rk3568_usb3otg_domain_init(void)
{
	volatile uint32_t *cru = (volatile uint32_t *)USBH_CRU_BASE;
	volatile uint32_t *grf0 =
		(volatile uint32_t *)USBH_USB2PHY0_GRF_BASE;
	volatile uint32_t *dwc3 = (volatile uint32_t *)USBH_XHCI0_BASE;
	uint32_t gctl, guctl1, g2phy, g3pipe, dcfg, rev;

	if (s_usb3otg_domain_done) {
		return;
	}

	if (!s_usb_bus_domain_done) {
		usb_bus_domain_once();
	}

	/* USB3OTG clock gates (otg0 + otg1 + the pipe family, in case the
	 * SATA combphy line has not run yet). Gate polarity: low-bit 1 =
	 * off, data 0 = enabled. */
	*(cru + USBH_CRU_CLKGATE_CON10 / 4U) =
		GRF_WR(USBH_CRU_CLKGATE_CON10_BITS, 0U);

	/* Assert + release SRST_USB3OTG0/1 and the USB2HOST resets (the same
	 * con14 pulse the usb2phy1 line uses; in this image it bounces
	 * nothing). ATF leaves the dwc3 cores in reset after cold power-on
	 * - without this pulse GSNPSID reads 0. */
	*(cru + USBH_CRU_SOFTRST_CON9 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON9_BITS, USBH_CRU_SOFTRST_CON9_BITS);
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS,
		       USBH_CRU_SOFTRST_CON14_BITS);
	usb_udelay(100);
	*(cru + USBH_CRU_SOFTRST_CON9 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON9_BITS, 0U);
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS, 0U);
	usb_udelay(100);

	/* Release-only: usb2phy1 POR/port resets + its GRF pclk (the panel
	 * group; harmless in an xHCI image, NetBSD does it in the same
	 * shared domain sequence). */
	*(cru + USBH_CRU_SOFTRST_CON29 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON29_BITS, 0U);
	*(cru + USBH_CRU_SOFTRST_CON28 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON28_BITS, 0U);

	/* usb2phy0 port GRF: 480m clock output, otg-port host role, host-port
	 * suspend release - the KI-006 measured working values, 2ms settle
	 * per port write (NetBSD rk_usb2phy_port_write). */
	*(grf0 + USBH_USB2PHY_GRF_CLKOUT_CON2 / 4U) =
		GRF_WR(USBH_USB2PHY_CLKOUT_WE, 0U);
	usb_udelay(100);
	*(grf0 + USBH_USB2PHY_GRF_OTG_CON0 / 4U) =
		GRF_WR(USBH_USB2PHY_OTG_SUS_MASK, USBH_USB2PHY_OTG_SUS_VAL);
	usb_udelay(2000);
	*(grf0 + USBH_USB2PHY_GRF_HOST_CON1 / 4U) =
		GRF_WR(USBH_USB2PHY_HOST_SUS_MASK, USBH_USB2PHY_HOST_SUS_VAL);
	usb_udelay(2000);

	/* --- dwc3_fdt.c: soft_reset ------------------------------------- */
	/* Put the core, then both PHYs, in reset; settle 100ms each way.
	 * This is safe here (and was not in the D36 experiments) because the
	 * CRU SRST pulse above has already taken the core to a cold state -
	 * the NetBSD order, not a reset of a running controller. */
	dwc3[DWC3_GCTL / 4U] |= DWC3_GCTL_CORESOFTRESET;
	dwc3[DWC3_GUSB3PIPECTL / 4U] |= DWC3_GUSB3PIPECTL_PHYSOFTRST;
	dwc3[DWC3_GUSB2PHYCFG / 4U] |= DWC3_GUSB2PHYCFG_PHYSOFTRST;
	usb_osal_msleep(100);
	dwc3[DWC3_GUSB3PIPECTL / 4U] &= ~DWC3_GUSB3PIPECTL_PHYSOFTRST;
	dwc3[DWC3_GUSB2PHYCFG / 4U] &= ~DWC3_GUSB2PHYCFG_PHYSOFTRST;
	usb_osal_msleep(100);

	/* Take core out of reset (dwc3_fdt_soft_reset's closing CLR4 - 漏掉它
	 * 整个 xHCI 孔径留在软复位里读零, 2026-09-20 首板验实测)。 */
	dwc3[DWC3_GCTL / 4U] &= ~DWC3_GCTL_CORESOFTRESET;

	/* --- dwc3_fdt.c: enable_phy (rk3568 board dts quirk set) --------- */
	rev = dwc3[DWC3_GSNPSID / 4U] & DWC3_GSNPSID_REV_MASK;

	/* utmi_wide: 16-bit PHYIF + TRDTIM 5; dis_enblslpm / dis-u2-freeclk /
	 * dis_u2_susphy. (NetBSD applies the KI-012 fix here: ENBLSLPM is
	 * bit8, not bit0.) */
	g2phy = dwc3[DWC3_GUSB2PHYCFG / 4U];
	g2phy |= DWC3_GUSB2PHYCFG_PHYIF;
	g2phy &= ~DWC3_GUSB2PHYCFG_USBTRDTIM_MASK;
	g2phy |= DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT;
	g2phy &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
	g2phy &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;
	g2phy &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	dwc3[DWC3_GUSB2PHYCFG / 4U] = g2phy;

	/* GUSB3PIPECTL: clear UX_EXIT_PX; dis_u3_susphy; dis-del-phy-power-
	 * chg; dis-rxdet-inp3. */
	g3pipe = dwc3[DWC3_GUSB3PIPECTL / 4U];
	g3pipe &= ~DWC3_GUSB3PIPECTL_UX_EXIT_PX;
	g3pipe &= ~DWC3_GUSB3PIPECTL_SUSPENDUSB3;
	g3pipe &= ~DWC3_GUSB3PIPECTL_DEPOCHANGE;
	g3pipe |= DWC3_GUSB3PIPECTL_DISRXDETINP3;
	dwc3[DWC3_GUSB3PIPECTL / 4U] = g3pipe;

	/* HS-only: force the USB2 clock into the USB3 routing, or link
	 * training never completes (KI-012 root cause). NetBSD gates on
	 * rev >= 0x290a && maximum-speed = high-speed; this IP is 0x300a. */
	if (rev >= 0x290AU) {
		guctl1 = dwc3[DWC3_GUCTL1 / 4U];
		guctl1 |= DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK;
		dwc3[DWC3_GUCTL1 / 4U] = guctl1;
	}

	/* DCFG speed = high-speed (0). */
	dcfg = dwc3[DWC3_DCFG / 4U];
	dcfg &= ~DWC3_DCFG_SPEED_MASK;
	dwc3[DWC3_DCFG / 4U] = dcfg;

	/* --- dwc3_fdt.c: set_mode(host) ---------------------------------- */
	gctl = dwc3[DWC3_GCTL / 4U];
	gctl &= ~DWC3_GCTL_PRTCAP_MASK;
	gctl |= DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_HOST);
	dwc3[DWC3_GCTL / 4U] = gctl;

	/* The KI-012 comparison window (NetBSD prints the same four). */
	usbh_console_printf(
		"[USBH] usb3otg domain up: GCTL=0x%08x GUCTL1=0x%08x GUSB2PHYCFG0=0x%08x GUSB3PIPECTL0=0x%08x\r\n",
		dwc3[DWC3_GCTL / 4U], dwc3[DWC3_GUCTL1 / 4U],
		dwc3[DWC3_GUSB2PHYCFG / 4U], dwc3[DWC3_GUSB3PIPECTL / 4U]);

	s_usb3otg_domain_done = true;
}
