/*
 * @file
 * @brief RK3568 USB3 socket-group domain bring-up and the xHCI host
 * composition (the dwc3_fdt.c role).
 *
 * The register sequence is the net_80211 line's usbh_platform.c
 * (board-proven: D38 xHCI enumeration from a cold USB domain, D50
 * four-bus build) merged with the lab NetBSD tree's dwc3_fdt.c: the
 * shared bus domain (PD_PIPE + PHY reference clocks + VBUS) runs
 * first, then the USB3OTG clock gates, the CRU SRST pulse that takes
 * the ATF-held DWC3 cores to a cold state (without it GSNPSID reads
 * 0), and the usb2phy0 port GRF.  Per instance the DWC3 core is
 * reconfigured - soft reset closed with the GCTL.CORESOFTRESET clear
 * (skipping it leaves the whole xHCI aperture reading zero), PHY
 * quirks, PRTCAP=host - and then the ehci_fdt-style composition:
 * struct xhci_softc, the interrupt armed, xhci_init(), and both buses
 * (USB3 + USB2) handed to the NetBSD usb driver through config_found.
 *
 * Ordering discipline (D50): the SRST pulses here reset shared USB
 * blocks, so every domain must be brought up before any HCD runs -
 * usb_platform_init() calls usb_usb3_domain_init() and the usb2phy1
 * domain before either attach.
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

/* the same prologue xhci.c carries: xhcivar.h expects the usbdi world
 * (usb_dma_t, the bus types) first */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/pmf.h>
#include <sys/pool.h>
#include <sys/bus.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_mem.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usb_quirks.h>
#include <dev/usb/usbhist.h>
#include <dev/usb/xhcireg.h>
#include <dev/usb/xhcivar.h>

#include "irq_ctrl.h"

#include "board.h"

#include "usb_board.h"
#include "usb_dwc3.h"
#include "usb_platform.h"

/* the global DMA tag the compat bus.h documents (bsd_bus.c) */
extern bus_dma_tag_t wlan_bus_dma_tag;

static bool s_usb3_domain_done;

/* Microsecond busy-wait off the ARM generic timer (always on at EL1);
 * the same primitive usb_platform.c uses. */
static void usb_udelay(uint32_t usec)
{
	uint64_t start, now, ticks, freq;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
	ticks = (freq * (uint64_t) usec) / 1000000U;
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

/* ------------------------------------------------------------------
 * the USB3 socket-group domain (once per boot): CRU clock gates, the
 * SRST pulse, the usb2phy0 port GRF.  Register-exact net_80211 line.
 */
void usb_usb3_domain_init(void)
{
	volatile uint32_t *cru = (volatile uint32_t *) USBH_CRU_BASE;
	volatile uint32_t *grf0 =
		(volatile uint32_t *) USBH_USB2PHY0_GRF_BASE;

	if (s_usb3_domain_done) {
		return;
	}

	/* PD_PIPE bus island + the PHY reference clocks + VBUS (the
	 * USB3 socket group hangs off GPIO3_A1; a cold VBUS ramp is
	 * what makes pre-inserted devices enumerate, KI-006). */
	usb_bus_domain_once();

	/* USB3OTG clock gates (con10: otg0 + otg1 + the pipe family, in
	 * case the SATA combphy line has not run yet).  Gate polarity:
	 * low bit 1 = off, data 0 = enabled. */
	*(cru + USBH_CRU_CLKGATE_CON10 / 4U) =
		GRF_WR(USBH_CRU_CLKGATE_CON10_BITS, 0U);

	/* Assert + release the SRST pulse: con9 (USB3OTG0/1) plus the
	 * same con14 (USB2HOST) pulse the usb2phy1 line uses - run
	 * here, ahead of both attaches, it bounces nothing (D50). */
	*(cru + USBH_CRU_SOFTRST_CON9 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON9_BITS,
		       USBH_CRU_SOFTRST_CON9_BITS);
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS,
		       USBH_CRU_SOFTRST_CON14_BITS);
	usb_udelay(100);
	*(cru + USBH_CRU_SOFTRST_CON9 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON9_BITS, 0U);
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS, 0U);
	usb_udelay(100);

	/* Release-only: usb2phy1 POR/port resets + its GRF pclk. */
	*(cru + USBH_CRU_SOFTRST_CON29 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON29_BITS, 0U);
	*(cru + USBH_CRU_SOFTRST_CON28 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON28_BITS, 0U);

	/* usb2phy0 port GRF: 480m clock output, otg-port host role,
	 * host-port suspend release - the KI-006 measured working
	 * values, 2 ms settle per port write (the NetBSD
	 * rk_usb2phy_port_write cadence).  Both usb2phy0 lanes are
	 * shared domain resources: the otg-port lane feeds the
	 * 0xFCC00000 instance, the host-port lane the 0xFD000000 one. */
	*(grf0 + USBH_USB2PHY_GRF_CLKOUT_CON2 / 4U) =
		GRF_WR(USBH_USB2PHY_CLKOUT_WE, 0U);
	usb_udelay(100);
	*(grf0 + USBH_USB2PHY_GRF_OTG_CON0 / 4U) =
		GRF_WR(USBH_USB2PHY_OTG_SUS_MASK, USBH_USB2PHY_OTG_SUS_VAL);
	usb_udelay(2000);
	*(grf0 + USBH_USB2PHY_GRF_HOST_CON1 / 4U) =
		GRF_WR(USBH_USB2PHY_HOST_SUS_MASK, USBH_USB2PHY_HOST_SUS_VAL);
	usb_udelay(2000);

	s_usb3_domain_done = true;
}

/* ------------------------------------------------------------------
 * dwc3_fdt.c's soft_reset + enable_phy + set_mode for one core.  The
 * DWC3 global registers sit above the xHCI aperture (usb_board.h: core
 * globals at +0xC100).
 */
static void usb_xhci_dwc3_host_init(uintptr_t base)
{
	volatile uint32_t *dwc3 = (volatile uint32_t *) base;
	uint32_t gctl, guctl1, g2phy, g3pipe, dcfg, rev;

	/* Core, then both PHYs, into reset; settle 100 ms each way.
	 * Safe here (and was not in the D36 experiments) because the
	 * CRU SRST pulse has already taken the core to a cold state -
	 * the NetBSD order, not a reset of a running controller. */
	dwc3[DWC3_GCTL / 4U] |= DWC3_GCTL_CORESOFTRESET;
	dwc3[DWC3_GUSB3PIPECTL / 4U] |= DWC3_GUSB3PIPECTL_PHYSOFTRST;
	dwc3[DWC3_GUSB2PHYCFG / 4U] |= DWC3_GUSB2PHYCFG_PHYSOFTRST;
	osDelay(100);
	dwc3[DWC3_GUSB3PIPECTL / 4U] &= ~DWC3_GUSB3PIPECTL_PHYSOFTRST;
	dwc3[DWC3_GUSB2PHYCFG / 4U] &= ~DWC3_GUSB2PHYCFG_PHYSOFTRST;
	osDelay(100);

	/* Take the core out of reset - dwc3_fdt's soft reset closing
	 * clear.  Skipped, the whole xHCI register window stays inside
	 * the soft reset and reads zero. */
	dwc3[DWC3_GCTL / 4U] &= ~DWC3_GCTL_CORESOFTRESET;

	rev = dwc3[DWC3_GSNPSID / 4U] & DWC3_GSNPSID_REV_MASK;
	printf("xhci0: dwc3 @%08x GSNPSID rev %u.%03x\n", (uint32_t) base,
	    (unsigned) (rev >> 12U), (unsigned) (rev & 0xfffU));

	/* utmi_wide: 16-bit PHYIF + TRDTIM 5; dis_enblslpm (the KI-012
	 * fix: ENBLSLPM is bit8, not bit0) / dis-u2-freeclk /
	 * dis_u2_susphy. */
	g2phy = dwc3[DWC3_GUSB2PHYCFG / 4U];
	g2phy |= DWC3_GUSB2PHYCFG_PHYIF;
	g2phy &= ~DWC3_GUSB2PHYCFG_USBTRDTIM_MASK;
	g2phy |= DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT;
	g2phy &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;
	g2phy &= ~DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS;
	g2phy &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	dwc3[DWC3_GUSB2PHYCFG / 4U] = g2phy;

	/* GUSB3PIPECTL: clear UX_EXIT_PX; dis_u3_susphy; dis-del-phy-
	 * power-chg; dis-rxdet-inp3. */
	g3pipe = dwc3[DWC3_GUSB3PIPECTL / 4U];
	g3pipe &= ~DWC3_GUSB3PIPECTL_UX_EXIT_PX;
	g3pipe &= ~DWC3_GUSB3PIPECTL_SUSPENDUSB3;
	g3pipe &= ~DWC3_GUSB3PIPECTL_DEPOCHANGE;
	g3pipe |= DWC3_GUSB3PIPECTL_DISRXDETINP3;
	dwc3[DWC3_GUSB3PIPECTL / 4U] = g3pipe;

	/* HS-only: force the USB2 clock into the USB3 routing, or link
	 * training never completes (the KI-012 root cause).  NetBSD
	 * gates on rev >= 0x290a && maximum-speed = high-speed; this IP
	 * is 0x300a. */
	if (rev >= 0x290AU) {
		guctl1 = dwc3[DWC3_GUCTL1 / 4U];
		guctl1 |= DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK;
		dwc3[DWC3_GUCTL1 / 4U] = guctl1;
	}

	/* DCFG speed = high-speed (0). */
	dcfg = dwc3[DWC3_DCFG / 4U];
	dcfg &= ~DWC3_DCFG_SPEED_MASK;
	dwc3[DWC3_DCFG / 4U] = dcfg;

	/* dwc3_fdt set_mode(host): the upper-port instance is the OTG
	 * controller - without the forced PRTCAP nothing enumerates. */
	gctl = dwc3[DWC3_GCTL / 4U];
	gctl &= ~DWC3_GCTL_PRTCAP_MASK;
	gctl |= DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_HOST);
	dwc3[DWC3_GCTL / 4U] = gctl;

	/* the KI-012 comparison window (NetBSD prints the same four) */
	printf("xhci0: dwc3 host mode up: GCTL=%08x GUCTL1=%08x "
	    "GUSB2PHYCFG0=%08x GUSB3PIPECTL0=%08x\n",
	    dwc3[DWC3_GCTL / 4U], dwc3[DWC3_GUCTL1 / 4U],
	    dwc3[DWC3_GUSB2PHYCFG / 4U], dwc3[DWC3_GUSB3PIPECTL / 4U]);
}

/* ------------------------------------------------------------------
 * xHCI composition (the ehci_fdt.c role, with twice the buses)
 */

static struct xhci_softc usb_xhci_sc;
static struct device usb_xhci_dev;
static bool s_usb_xhci_attached;

/* the CMSIS IRQ front hands no argument; keep the one softc we armed */
static struct xhci_softc *usb_xhci_isr_sc;
volatile unsigned usb_xhci_irq_count;

static void usb_xhci_isr(void)
{
	usb_xhci_irq_count++;
	(void) xhci_intr(usb_xhci_isr_sc);
}

int usb_xhci_attach(void)
{
	struct xhci_softc *sc = &usb_xhci_sc;
	struct device *dev = &usb_xhci_dev;
	uintptr_t base = (uintptr_t) USBH_XHCI1_BASE;
	uint32_t irq = USBH_XHCI1_IRQ;
	device_t child;
	int error;

	usb_usb3_domain_init();

	/* the DWC3 global register sequence first: xhci_init's very
	 * first CAPLENGTH read lands in the xHCI aperture, which the
	 * core only exposes once it has left the soft reset */
	usb_xhci_dwc3_host_init(base);

	snprintf(dev->dv_xname, sizeof(dev->dv_xname), "xhci0");
	{
		extern struct cfdriver xhci_cd;

		if (0U < xhci_cd.cd_ndevs) {
			xhci_cd.cd_devs[0] = dev;
		}
	}
	dev->dv_unit = 0;

	memset(sc, 0, sizeof(*sc));
	sc->sc_dev = dev;
	dev->dv_private = sc;
	sc->sc_bus.ub_hcpriv = sc;
	sc->sc_bus.ub_dmatag = wlan_bus_dma_tag;
	sc->sc_bus.ub_revision = USBREV_3_0;
	/* the DWC3 register window: the xHCI aperture at +0, the core
	 * globals this file touched at +0xC100 */
	sc->sc_ios = 0x100000UL;
	sc->sc_iot = (bus_space_tag_t) { 0 };
	sc->sc_ioh = (bus_space_handle_t) base;

	/* arm the handler but keep the line masked: the domain ramp posts
	 * port-change events into the ring before the usb children exist,
	 * and the ISR schedules its softint through the usbus shells -
	 * the first interrupt must only arrive after config_found, and
	 * it then drains everything queued up */
	IRQ_SetHandler((IRQn_ID_t) irq, usb_xhci_isr);
	usb_xhci_isr_sc = sc;
	IRQ_SetPriority((IRQn_ID_t) irq, BOARD_IRQ_PRIORITY_API_CALL_RAW);
	printf("xhci0: interrupting on INTID %u\n", irq);

	error = xhci_init(sc);
	if (error != 0) {
		printf("xhci0: xhci_init failed, error = %d\n", error);
		IRQ_Disable((IRQn_ID_t) irq);
		return error;
	}

	/* the xHCI presents two usb buses (USB3 + USB2); dwc3_fdt.c
	 * config_founds both */
	child = config_found(dev, &sc->sc_bus, usbctlprint,
	    CFARGS(.iattr = "usbus"));
	if (child == NULL) {
		printf("xhci0: usbus attach failed\n");
		return ENODEV;
	}
	sc->sc_child = child;
	sc->sc_child2 = config_found(dev, &sc->sc_bus2, usbctlprint,
	    CFARGS(.iattr = "usbus"));

	/* register both buses (usb_platform.h); the host-abstraction backend
	 * walks this registry, and an unrecorded bus would be invisible to it
	 * while still enumerating devices - the silent half-failure this
	 * registry exists to prevent */
	usb_platform_bus_record(&sc->sc_bus);
	usb_platform_bus_record(&sc->sc_bus2);

	/* both usbuses attached - unmask and let the first interrupt
	 * drain the queued port-change events */
	IRQ_Enable((IRQn_ID_t) irq);

	s_usb_xhci_attached = true;
	return 0;
}

/* ------------------------------------------------------------------
 * diagnostics
 *
 * The rules the EHCI dump learned the hard way: capability registers
 * live at sc_cbh (base + 0), operational ones at sc_obh (base +
 * CAPLENGTH), runtime ones at sc_rbh, and anything the DMA engine
 * wrote has to be invalidated before the CPU reads it.
 */
void usb_xhci_dump(void)
{
	struct xhci_softc *sc = &usb_xhci_sc;
	uintptr_t cap = (uintptr_t) sc->sc_cbh;
	uintptr_t op = (uintptr_t) sc->sc_obh;
	uintptr_t rt = (uintptr_t) sc->sc_rbh;
	uint32_t dboff = *(volatile uint32_t *) (cap + XHCI_DBOFF);
	uint32_t hcs1, hcs2, hcc;
	int i;

	if (!s_usb_xhci_attached) {
		printf("usb: xhci not started\n");
		return;
	}

	hcs1 = *(volatile uint32_t *) (cap + XHCI_HCSPARAMS1);
	hcs2 = *(volatile uint32_t *) (cap + XHCI_HCSPARAMS2);
	hcc = *(volatile uint32_t *) (cap + XHCI_HCCPARAMS);
	printf("usb: xhci hcs1=%08x ports=%u slots=%u intrs=%u "
	    "hcs2=%08x spbuf=%u hcc=%08x\n",
	    hcs1,
	    (unsigned) XHCI_HCS1_MAXPORTS(hcs1),
	    (unsigned) XHCI_HCS1_MAXSLOTS(hcs1),
	    (unsigned) XHCI_HCS1_MAXINTRS(hcs1),
	    hcs2, (unsigned) XHCI_HCS2_MAXSPBUF(hcs2), hcc);
	printf("usb: xhci cmd=%08x sts=%08x pagesize=%08x dboff=%08x "
	    "db0=%08x db1=%08x\n",
	    *(volatile uint32_t *) (op + XHCI_USBCMD),
	    *(volatile uint32_t *) (op + XHCI_USBSTS),
	    *(volatile uint32_t *) (op + XHCI_PAGESIZE),
	    dboff,
	    *(volatile uint32_t *) ((uintptr_t) sc->sc_dbh + 0U * 4U),
	    *(volatile uint32_t *) ((uintptr_t) sc->sc_dbh + 1U * 4U));
	printf("usb: xhci crcr=%016llx dcbaap=%08x config=%08x\n",
	    (unsigned long long) *(volatile uint64_t *) (op + XHCI_CRCR),
	    *(volatile uint32_t *) (op + XHCI_DCBAAP),
	    *(volatile uint32_t *) (op + XHCI_CONFIG));
	printf("usb: xhci iman=%08x imod=%08x erstsz=%u erstba=%08x "
	    "erdp=%08x irq_count=%u\n",
	    *(volatile uint32_t *) (rt + XHCI_IMAN(0)),
	    *(volatile uint32_t *) (rt + XHCI_IMOD(0)),
	    (unsigned) (*(volatile uint32_t *) (rt + XHCI_ERSTSZ(0)) &
		0xffffU),
	    *(volatile uint32_t *) (rt + XHCI_ERSTBA(0)),
	    *(volatile uint32_t *) (rt + XHCI_ERDP(0)),
	    usb_xhci_irq_count);

	/* what the controller is being handed: the command ring's first
	 * TRBs and the event-ring segment table entry, invalidated
	 * first so DRAM (not a stale cache line) is printed */
	{
		volatile struct xhci_soft_trb *cr =
		    (volatile struct xhci_soft_trb *)
			KERNADDR(&sc->sc_cr->xr_dma, 0);

		board_dcache_invalidate((uintptr_t) cr, 4U * XHCI_TRB_SIZE);
		for (i = 0; i < 4; i++) {
			printf("usb: xhci cr[%d] %08x%08x %08x %08x\n", i,
			    (unsigned) (cr[i].trb_0 >> 32),
			    (unsigned) cr[i].trb_0,
			    (unsigned) cr[i].trb_2, (unsigned) cr[i].trb_3);
		}
	}
	{
		volatile uint32_t *erst =
		    (volatile uint32_t *) KERNADDR(&sc->sc_eventst_dma, 0);

		board_dcache_invalidate((uintptr_t) erst, 16U);
		printf("usb: xhci erst[0] %08x%08x %08x\n",
		    erst[1], erst[0], erst[2]);

		/* the event ring itself: a completion sitting here that
		 * the softint never consumed points at our side; an empty
		 * ring points at the controller not running the command */
		{
			volatile struct xhci_soft_trb *er =
			    (volatile struct xhci_soft_trb *)
				KERNADDR(&sc->sc_er->xr_dma, 0);

			board_dcache_invalidate((uintptr_t) er,
			    4U * XHCI_TRB_SIZE);
			for (i = 0; i < 4; i++) {
				printf("usb: xhci er[%d] %08x%08x %08x "
				    "%08x\n", i,
				    (unsigned) (er[i].trb_0 >> 32),
				    (unsigned) er[i].trb_0,
				    (unsigned) er[i].trb_2,
				    (unsigned) er[i].trb_3);
			}
		}
	}
	{
		volatile uint64_t *dcbaa =
		    (volatile uint64_t *) KERNADDR(&sc->sc_dcbaa_dma, 0);

		board_dcache_invalidate((uintptr_t) dcbaa, 3U * 8U);
		printf("usb: xhci dcbaa[0..2] %016llx %016llx %016llx\n",
		    (unsigned long long) dcbaa[0],
		    (unsigned long long) dcbaa[1],
		    (unsigned long long) dcbaa[2]);
	}

	for (i = 0; i < sc->sc_maxports; i++) {
		uint32_t v = *(volatile uint32_t *)
		    (op + XHCI_PORTSC(i));

		printf("usb: xhci portsc%d=%08x ccs=%u csc=%u ped=%u "
		    "pls=%u speed=%u\n", i + 1, v,
		    (v & XHCI_PS_CCS) ? 1U : 0U,
		    (v & XHCI_PS_CSC) ? 1U : 0U,
		    (v & XHCI_PS_PED) ? 1U : 0U,
		    (unsigned) XHCI_PS_PLS_GET(v),
		    (unsigned) XHCI_PS_SPEED_GET(v));
	}
}

/* raw register rows over the three windows (the ehci reg_dump habit):
 * when a bring-up misbehaves the derived fields above say what the
 * driver believes, these say what the silicon reports */
void usb_xhci_reg_dump(void)
{
	struct xhci_softc *sc = &usb_xhci_sc;
	uintptr_t cap = (uintptr_t) sc->sc_cbh;
	uintptr_t op = (uintptr_t) sc->sc_obh;
	uintptr_t rt = (uintptr_t) sc->sc_rbh;
	int i;

	if (!s_usb_xhci_attached) {
		printf("usb: xhci not started\n");
		return;
	}

	printf("usb: xhci cap[00]=%08x[04]=%08x[08]=%08x[0c]=%08x\n",
	    *(volatile uint32_t *) (cap + 0x00),
	    *(volatile uint32_t *) (cap + 0x04),
	    *(volatile uint32_t *) (cap + 0x08),
	    *(volatile uint32_t *) (cap + 0x0c));
	printf("usb: xhci cap[10]=%08x[14]=%08x[18]=%08x[1c]=%08x\n",
	    *(volatile uint32_t *) (cap + 0x10),
	    *(volatile uint32_t *) (cap + 0x14),
	    *(volatile uint32_t *) (cap + 0x18),
	    *(volatile uint32_t *) (cap + 0x1c));
	for (i = 0; i < 0x60; i += 0x10) {
		printf("usb: xhci op[%02x]=%08x[%02x]=%08x[%02x]=%08x"
		    "[%02x]=%08x\n", i,
		    *(volatile uint32_t *) (op + i), i + 4,
		    *(volatile uint32_t *) (op + i + 4), i + 8,
		    *(volatile uint32_t *) (op + i + 8), i + 0xc,
		    *(volatile uint32_t *) (op + i + 0xc));
	}
	printf("usb: xhci rt mfin=%08x iman=%08x imod=%08x "
	    "erstsz=%08x\n",
	    *(volatile uint32_t *) rt,
	    *(volatile uint32_t *) (rt + XHCI_IMAN(0)),
	    *(volatile uint32_t *) (rt + XHCI_IMOD(0)),
	    *(volatile uint32_t *) (rt + XHCI_ERSTSZ(0)));
	printf("usb: xhci rt erstba=%08x%08x erdp=%08x%08x\n",
	    *(volatile uint32_t *) (rt + XHCI_ERSTBA(0) + 4),
	    *(volatile uint32_t *) (rt + XHCI_ERSTBA(0)),
	    *(volatile uint32_t *) (rt + XHCI_ERDP(0) + 4),
	    *(volatile uint32_t *) (rt + XHCI_ERDP(0)));
	printf("usb: xhci db[0]=%08x db[1]=%08x irq_count=%u\n",
	    *(volatile uint32_t *) ((uintptr_t) sc->sc_dbh + 0U),
	    *(volatile uint32_t *) ((uintptr_t) sc->sc_dbh + 4U),
	    usb_xhci_irq_count);
}
