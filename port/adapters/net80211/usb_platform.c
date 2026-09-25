/*
 * @file
 * @brief RK3568 USB platform bring-up and the EHCI host composition.
 *
 * The register sequence is the net_80211 line's usbh_platform.c
 * (board-proven: D35 EHCI enumeration, M7 urtwn attach), ported from
 * its CherryUSB harness to this one: the usb2phy1 domain for the panel
 * EHCI roots, then the ehci_fdt.c role - compose struct ehci_softc,
 * ehci_init(), arm the interrupt, and hand the bus to the NetBSD usb
 * driver through config_found.
 *
 * @date 25.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

/* the same prologue ehci.c carries: ehcivar.h expects the usbdi world
 * (usb_dma_t, the bus types, ehcireg's descriptor shapes) first */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/bus.h>
#include <sys/pool.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_mem.h>
#include <dev/usb/usbdivar.h>
#include <dev/usb/usb_quirks.h>
#include <dev/usb/ehcireg.h>
#include <dev/usb/ehcivar.h>

#include "irq_ctrl.h"

#include "board.h"

#include "usb_board.h"
#include "usb_platform.h"

/* the global DMA tag the compat bus.h documents (bsd_bus.c) */
extern bus_dma_tag_t wlan_bus_dma_tag;

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

/* PD_PIPE power island on + PHY reference clock gates + VBUS enable.
 * Bus island: request idle, wait for the ack, ungate the domain, wait
 * for the power-down status to clear (the standalone-line sequence). */
static void usb_bus_domain_once(void)
{
	volatile uint32_t *pmu = (volatile uint32_t *)USBH_PMU_BASE;
	volatile uint32_t *pmucru = (volatile uint32_t *)USBH_PMUCRU_BASE;
	volatile uint32_t *ddr = (volatile uint32_t *)(USBH_GPIO3_BASE +
						       USBH_GPIO3_SWPORT_DDR);
	volatile uint32_t *dr = (volatile uint32_t *)(USBH_GPIO3_BASE +
						      USBH_GPIO3_SWPORT_DR);
	uint32_t i;

	/* PD_PIPE bus idle request release (write-enable only, data 0),
	 * then wait for the ack to clear. */
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

	/* PHY reference clocks (usbphy0 + usbphy1 + ref24m): CRU clkgate
	 * polarity is low-bit 1 = gate OFF, so data 0 turns them on. */
	*(pmucru + USBH_PMUCRU_CLKGATE_CON2 / 4U) =
		GRF_WR(USBH_PMUCRU_USBPHY_GATES, 0U);

	/* VBUS: GPIO3_A0/A1 as output-high (hi-word = direction write, low
	 * word = data write; both set the pin). Without the pull-up the
	 * ports have no power at all (KI-004), and a cold VBUS ramp is
	 * what makes pre-inserted devices enumerate (KI-006). */
	*ddr = GRF_WR(USBH_VBUS_PINS, USBH_VBUS_PINS);
	*dr = GRF_WR(USBH_VBUS_PINS, USBH_VBUS_PINS);

	s_usb_bus_domain_done = true;
}

/* The usb2phy1 domain for the panel EHCI roots (both roots share the
 * one PHY domain; runs once per boot). Register-exact net_80211 line. */
static void usb_usb2phy1_domain_init(void)
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

	/* PMUCRU: the usb2phy1 reference bit on top of the bus domain
	 * ungates (clkgate data 0 = clock enabled). */
	*(pmucru + USBH_PMUCRU_CLKGATE_CON2 / 4U) =
		GRF_WR(USBH_PMUCRU_USBPHY_GATES, 0U);

	/* USB2HOST0/1 + their ARB/UTMI interfaces: assert, settle, release. */
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS,
		       USBH_CRU_SOFTRST_CON14_BITS);
	usb_udelay(100);
	*(cru + USBH_CRU_SOFTRST_CON14 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON14_BITS, 0U);
	usb_udelay(100);

	/* PHY1 POR/port reset and the GRF pclk: release only, never
	 * assert - asserting the POR would drop a PHY the other bus uses. */
	*(cru + USBH_CRU_SOFTRST_CON29 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON29_BITS, 0U);
	usb_udelay(100);
	*(cru + USBH_CRU_SOFTRST_CON28 / 4U) =
		GRF_WR(USBH_CRU_SOFTRST_CON28_BITS, 0U);
	usb_udelay(100);

	/* Port GRF: otg-port then host-port suspend release, then open the
	 * 480m UTMI clock output; 2 ms settle per write (NetBSD
	 * rk_usb2phy_port_write cadence). */
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

/* ------------------------------------------------------------------
 * EHCI composition (the ehci_fdt.c role)
 */

static struct ehci_softc usb_ehci_sc[USBH_EHCI_NUM];
static struct device usb_ehci_dev[USBH_EHCI_NUM];

/* the CMSIS IRQ front hands no argument; keep the one softc we armed */
static struct ehci_softc *usb_ehci_isr_sc;
volatile unsigned usb_ehci_irq_count;
volatile unsigned usb_ehci_irq_last_sts;

static void usb_ehci_isr(void)
{
	usb_ehci_irq_count++;
	usb_ehci_irq_last_sts = EREAD4(usb_ehci_isr_sc, EHCI_USBSTS);
	(void) ehci_intr(usb_ehci_isr_sc);
}

static uint32_t usb_ehci_irq(int id)
{
	return (id == 0) ? USBH_EHCI0_IRQ : USBH_EHCI1_IRQ;
}

static int usb_ehci_attach(int id)
{
	struct ehci_softc *sc = &usb_ehci_sc[id];
	struct device *dev = &usb_ehci_dev[id];
	uintptr_t base = (uintptr_t) USBH_EHCI_BASE(id);
	uint32_t irq = usb_ehci_irq(id);
	device_t child;
	int error;

	usb_usb2phy1_domain_init();

	snprintf(dev->dv_xname, sizeof(dev->dv_xname), "ehci%d", id);
	{
		extern struct cfdriver ehci_cd;

		if (id < ehci_cd.cd_ndevs) {
			ehci_cd.cd_devs[id] = dev;
		}
	}
	dev->dv_unit = id;

	memset(sc, 0, sizeof(*sc));
	sc->sc_dev = dev;
	dev->dv_private = sc;
	sc->sc_bus.ub_hcpriv = sc;
	sc->sc_bus.ub_dmatag = wlan_bus_dma_tag;
	sc->sc_bus.ub_revision = USBREV_2_0;
	sc->sc_bus.ub_hctype = USBHCTYPE_EHCI;
	sc->sc_ncomp = 0;
	/* the DWC core carries an embedded transaction translator: FS/LS
	 * devices (the CH334P hub) enumerate straight on the root port,
	 * exactly what U-Boot's ehci does (ehci_fdt's
	 * has-transaction-translator) */
	sc->sc_flags = EHCIF_ETTF;
	sc->sc_size = 0x10000UL;
	sc->iot = (bus_space_tag_t) { 0 };
	sc->ioh = (bus_space_handle_t) base;

	/* ehci_fdt order: CAPLENGTH + silence the controller before the
	 * interrupt line is armed */
	sc->sc_offs = EREAD1(sc, EHCI_CAPLENGTH);
	EOWRITE4(sc, EHCI_USBINTR, 0);

	IRQ_SetHandler((IRQn_ID_t) irq, usb_ehci_isr);
	usb_ehci_isr_sc = sc;
	IRQ_SetPriority((IRQn_ID_t) irq, BOARD_IRQ_PRIORITY_API_CALL_RAW);
	IRQ_Enable((IRQn_ID_t) irq);
	printf("ehci%d: interrupting on INTID %u\n", id, irq);

	error = ehci_init(sc);
	if (error != 0) {
		printf("ehci%d: ehci_init failed, error = %d\n", id, error);
		IRQ_Disable((IRQn_ID_t) irq);
		return error;
	}

	/* hand the bus to the usb driver (the generated ioconf would do
	 * the matching; our static table gates on the usbus attribute) */
	child = config_found(dev, &sc->sc_bus, usbctlprint,
	    CFARGS(.iattr = "usbus"));
	if (child == NULL) {
		printf("ehci%d: usbus attach failed\n", id);
		return ENODEV;
	}
	return 0;
}

void usb_platform_dump(void)
{
	struct ehci_softc *sc = &usb_ehci_sc[1];
	uint32_t sts, portsc;

	if (!s_usb2phy1_domain_done) {
		printf("usb: platform not started\n");
		return;
	}
	sts = EREAD4(sc, EHCI_USBSTS);
	portsc = EREAD4(sc, EHCI_PORTSC(1));
	printf("usb: irq_count=%u irq_last_sts=%08x\n",
	    usb_ehci_irq_count, usb_ehci_irq_last_sts);
	printf("usb: cmd=%08x sts=%08x intr=%08x portsc1=%08x\n",
	    EREAD4(sc, EHCI_USBCMD), sts, EREAD4(sc, EHCI_USBINTR),
	    portsc);
}

/* raw register window: identify the true layout (DWC EHCI cores put
 * the operational regs at CAPLENGTH, which is not 0 here) */
void usb_platform_reg_dump(void)
{
	struct ehci_softc *sc = &usb_ehci_sc[1];
	uint32_t base = (uintptr_t) USBH_EHCI_BASE(1);
	int i;

	for (i = 0; i < 0x80; i += 4) {
		printf("usb: reg[%02x] = %08x\n", i,
		    *(volatile uint32_t *) (base + i));
	}
	printf("usb: sc_offs=%02x\n", sc->sc_offs);
}

/* raw descriptor chase: what the (halted) DMA engine was pointed at */
void usb_platform_qh_dump(void)
{
	struct ehci_softc *sc = &usb_ehci_sc[1];
	uint32_t alist, plist;
	volatile uint32_t *qh;
	int i;

	if (!s_usb2phy1_domain_done) {
		printf("usb: platform not started\n");
		return;
	}
	alist = EREAD4(sc, EHCI_ASYNCLISTADDR);
	plist = EREAD4(sc, EHCI_PERIODICLISTBASE);
	printf("usb: asynclist=%08x periodiclist=%08x\n", alist, plist);

	qh = (volatile uint32_t *) (uintptr_t) (alist & ~31u);
	for (i = 0; i < 8; i++) {
		printf("usb: qh[%d] @%p = %08x %08x %08x %08x\n", i,
		    (void *) (qh + i * 4), qh[i * 4], qh[i * 4 + 1],
		    qh[i * 4 + 2], qh[i * 4 + 3]);
	}
	if (plist != 0) {
		volatile uint32_t *fl = (volatile uint32_t *) (uintptr_t) plist;

		for (i = 0; i < 4; i++) {
			printf("usb: fl[%d] = %08x\n", i, fl[i]);
		}
	}
}

int usb_platform_init(void)
{
	int error;

	/* the config worker drains config_interrupts/config_defer hooks -
	 * usb.c defers its whole attach (taskq threads + root hub) there */
	config_deferred_run();

	error = usb_ehci_attach(1); /* the panel Type-A pair (CH334P hub) */
	if (error != 0) {
		return error;
	}
	return 0;
}
