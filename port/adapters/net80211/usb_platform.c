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
#include <dev/usb/usbhist.h>

#include "irq_ctrl.h"

#include "board.h"

#include "usb_board.h"
#include "usb_platform.h"

/* the global DMA tag the compat bus.h documents (bsd_bus.c) */
extern bus_dma_tag_t wlan_bus_dma_tag;
extern void wlan_dma_pool_range(uintptr_t *base, size_t *len,
    uintptr_t *win_end);

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
 * for the power-down status to clear (the standalone-line sequence).
 * Shared: the xHCI line's USB3 domain sequence rides on it. */
void usb_bus_domain_once(void)
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
static bool s_usb_ehci_attached;

/* the CMSIS IRQ front hands no argument; keep the one softc we armed */
static struct ehci_softc *usb_ehci_isr_sc;
volatile unsigned usb_ehci_irq_count;
volatile unsigned usb_ehci_irq_last_sts;

static void usb_ehci_isr(void)
{
	usb_ehci_irq_count++;
	/* USBSTS is an operational register: an EREAD here reads the
	 * capability window (HCSPARAMS) and says nothing about the irq */
	usb_ehci_irq_last_sts = EOREAD4(usb_ehci_isr_sc, EHCI_USBSTS);
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
	/* The board shape, not a guess: this DTS carries no
	 * has-transaction-translator, so ehci_fdt.c leaves ETTF off and
	 * sets sc_ncomp = 1, letting ehci_init reconcile against HCSPARAMS
	 * (this silicon reports 1 companion / 1 port).  The NetBSD kernel
	 * that runs this board enumerates the panel ports in exactly this
	 * shape, and the CH334P is a 480 Mb/s hub whose own multiple TTs
	 * translate the FS/LS traffic behind it - the EHCI's embedded TT
	 * is never involved. */
	sc->sc_ncomp = 1;
	sc->sc_flags = 0;
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
	s_usb_ehci_attached = true;
	return 0;
}

/* ------------------------------------------------------------------
 * diagnostics
 *
 * Two rules learned the hard way on this core: capability registers are
 * read with EREAD and operational ones with EOREAD (sc_offs is 0x10, so
 * an EREAD of PORTSC lands 0x10 short), and anything the DMA engine
 * wrote (QH/qTD) has to be invalidated before the CPU reads it, or the
 * dump shows whatever cache line the driver last touched.
 */

static const char *usb_pspd_str(uint32_t portsc)
{
	switch (portsc & EHCI_PS_PSPD) {
	case EHCI_PS_PSPD_FS:
		return "full";
	case EHCI_PS_PSPD_LS:
		return "low";
	case EHCI_PS_PSPD_HS:
		return "high";
	default:
		return "rsvd";
	}
}

static void usb_portsc_line(const char *tag, uint32_t v)
{
	printf("usb: %s=%08x ccs=%u csc=%u pe=%u pec=%u pr=%u susp=%u pp=%u "
	    "po=%u pspd=%s ls=%u\n", tag, v,
	    (v & EHCI_PS_CS) ? 1U : 0U, (v & EHCI_PS_CSC) ? 1U : 0U,
	    (v & EHCI_PS_PE) ? 1U : 0U, (v & EHCI_PS_PEC) ? 1U : 0U,
	    (v & EHCI_PS_PR) ? 1U : 0U, (v & EHCI_PS_SUSP) ? 1U : 0U,
	    (v & EHCI_PS_PP) ? 1U : 0U, (v & EHCI_PS_PO) ? 1U : 0U,
	    usb_pspd_str(v), (unsigned) ((v & EHCI_PS_LS) >> 10));
}

void usb_platform_dump(void)
{
	struct ehci_softc *sc = &usb_ehci_sc[1];
	uint32_t hcsparams, hccparams;
	int i;

	/* the xHCI section speaks for itself when ehci is not attached
	 * (the diag build) */
	usb_xhci_dump();

	if (!s_usb_ehci_attached) {
		printf("usb: platform not started\n");
		return;
	}

	hcsparams = EREAD4(sc, EHCI_HCSPARAMS);
	hccparams = EREAD4(sc, EHCI_HCCPARAMS);
	{
		uintptr_t pool, win_end;
		size_t pool_len;

		wlan_dma_pool_range(&pool, &pool_len, &win_end);
		printf("usb: dma pool %08lx..%08lx window end %08lx\n",
		    (unsigned long) pool,
		    (unsigned long) (pool + pool_len),
		    (unsigned long) win_end);
	}
	printf("usb: irq_count=%u irq_last_sts=%08x\n",
	    usb_ehci_irq_count, usb_ehci_irq_last_sts);
	printf("usb: cmd=%08x sts=%08x intr=%08x frindex=%u cfgflag=%08x\n",
	    EOREAD4(sc, EHCI_USBCMD), EOREAD4(sc, EHCI_USBSTS),
	    EOREAD4(sc, EHCI_USBINTR), EOREAD4(sc, EHCI_FRINDEX),
	    EOREAD4(sc, EHCI_CONFIGFLAG));
	printf("usb: hcsparams=%08x ports=%u ppc=%u ncc=%u npcc=%u hccparams=%08x\n",
	    hcsparams, (unsigned) EHCI_HCS_N_PORTS(hcsparams),
	    (unsigned) EHCI_HCS_PPC(hcsparams), (unsigned) EHCI_HCS_N_CC(hcsparams),
	    (unsigned) EHCI_HCS_N_PCC(hcsparams), hccparams);
	printf("usb: sc_flags=%x ncomp=%u npcomp=%u noport=%d hasppc=%u offs=%02x "
	    "isreset=", sc->sc_flags, sc->sc_ncomp, sc->sc_npcomp,
	    sc->sc_noport, sc->sc_hasppc, sc->sc_offs);
	for (i = 1; i <= sc->sc_noport; i++) {
		printf("%d", sc->sc_isreset[i] != 0);
	}
	printf(" usbdebug=%d\n", usbdebug);
	for (i = 1; i <= sc->sc_noport; i++) {
		char tag[16];

		snprintf(tag, sizeof(tag), "portsc%d", i);
		usb_portsc_line(tag, EOREAD4(sc, EHCI_PORTSC(i)));
	}
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

/* one qTD: the state bits are the whole story of a stalled transfer -
 * a halted qTD with XACTERR is a device/bus error, with BUFERR the
 * controller could not move the data, still-active means the schedule
 * never got to it */
static void usb_qtd_line(const char *tag, const volatile ehci_qtd_t *td,
	int idx)
{
	uint32_t st = td->qtd_status;

	printf("usb: %s[%d] st=%08x %s%s%s%s%s%s bytes=%u pid=%u cerr=%u "
	    "next=%08x alt=%08x buf0=%08x\n", tag, idx, st,
	    (st & EHCI_QTD_ACTIVE) ? "act" : "---",
	    (st & EHCI_QTD_HALTED) ? " halt" : "",
	    (st & EHCI_QTD_BUFERR) ? " buferr" : "",
	    (st & EHCI_QTD_BABBLE) ? " babble" : "",
	    (st & EHCI_QTD_XACTERR) ? " xacterr" : "",
	    (st & EHCI_QTD_MISSEDMICRO) ? " miss" : "",
	    (unsigned) EHCI_QTD_GET_BYTES(st), (unsigned) EHCI_QTD_GET_PID(st),
	    (unsigned) EHCI_QTD_GET_CERR(st), td->qtd_next, td->qtd_altnext,
	    td->qtd_buffer[0]);
}

/* the async schedule walk: every QH the controller is working, with its
 * overlay and the qTD chain it is parked on */
void usb_platform_qh_dump(void)
{
	struct ehci_softc *sc = &usb_ehci_sc[1];
	uint32_t alist;
	volatile ehci_qh_t *qh;
	int i;

	if (!s_usb_ehci_attached) {
		printf("usb: platform not started\n");
		return;
	}

	alist = EOREAD4(sc, EHCI_ASYNCLISTADDR);
	printf("usb: asynclist=%08x periodiclist=%08x\n", alist,
	    EOREAD4(sc, EHCI_PERIODICLISTBASE));
	if (alist == 0) {
		return;
	}

	qh = (volatile ehci_qh_t *) (uintptr_t) (alist & ~0x1fU);
	for (i = 0; i < 8; i++) {
		uint32_t link, endp, endphub, cur;
		int t;

		board_dcache_invalidate((uintptr_t) qh, sizeof(*qh));
		link = qh->qh_link;
		endp = qh->qh_endp;
		endphub = qh->qh_endphub;
		cur = qh->qh_curqtd;
		printf("usb: qh[%d]@%08x link=%08x%s hrecl=%u ctl=%u addr=%u ep=%u "
		    "eps=%u mpl=%u nrl=%u huba=%u port=%u cur=%08x\n", i,
		    (unsigned) (uintptr_t) qh, link,
		    (link & EHCI_LINK_TERMINATE) ? "(T)" : "",
		    (unsigned) EHCI_QH_GET_HRECL(endp),
		    (unsigned) EHCI_QH_GET_CTL(endp),
		    (unsigned) EHCI_QH_GET_ADDR(endp),
		    (unsigned) EHCI_QH_GET_ENDPT(endp),
		    (unsigned) EHCI_QH_GET_EPS(endp),
		    (unsigned) EHCI_QH_GET_MPL(endp),
		    (unsigned) EHCI_QH_GET_NRL(endp),
		    (unsigned) EHCI_QH_GET_HUBA(endphub),
		    (unsigned) EHCI_QH_GET_PORT(endphub), cur);
		usb_qtd_line("usb: overlay", &qh->qh_qtd, 0);

		for (t = 0; t < 6; t++) {
			volatile ehci_qtd_t *td = (volatile ehci_qtd_t *)
			    (uintptr_t) (cur & ~0x1fU);
			uint32_t next;

			if (td == NULL) {
				break;
			}
			if (td != &qh->qh_qtd) {
				board_dcache_invalidate((uintptr_t) td,
				    sizeof(*td));
			}
			next = td->qtd_next;
			if (td != &qh->qh_qtd) {
				usb_qtd_line("usb: qtd", td, t);
			}
			if (next & EHCI_LINK_TERMINATE) {
				break;
			}
			cur = next;
		}

		if ((link & EHCI_LINK_TERMINATE) ||
		    EHCI_LINK_TYPE(link) != EHCI_LINK_QH) {
			break;
		}
		qh = (volatile ehci_qh_t *) (uintptr_t) (link & ~0x1fU);
	}
}

/* the usb history ring (kernhist): every state transition the imported
 * core logged, oldest first */
void usb_platform_hist_dump(unsigned int max)
{
	wlan_kernhist_dump(&usbhist, max);
}

/* Measure the two wait primitives the EHCI core rides on.  usb_delay_ms
 * (the 250 ms port-reset hold) goes through kpause/tsleep in this port,
 * and a short wait there would break the reset while leaving the PHY
 * looking perfectly healthy - so print requested against measured. */
void usb_platform_delay_test(void)
{
	static const unsigned int ms_list[] = { 1, 20, 50, 250 };
	static const unsigned int us_list[] = { 100, 1000, 2000 };
	unsigned long long t0, t1;
	size_t i;

	if (!s_usb_ehci_attached) {
		printf("usb: delaytest needs the platform (wlan start first)\n");
		return;
	}

	printf("usb: delaytest hz=%d tickfreq=%u\n", hz,
	    (unsigned) osKernelGetTickFreq());

	for (i = 0; i < sizeof(us_list) / sizeof(us_list[0]); i++) {
		t0 = wlan_kernhist_now_us();
		delay(us_list[i]);
		t1 = wlan_kernhist_now_us();
		printf("usb: delay(%u us) -> %llu us\n", us_list[i], t1 - t0);
	}
	for (i = 0; i < sizeof(ms_list) / sizeof(ms_list[0]); i++) {
		t0 = wlan_kernhist_now_us();
		usb_delay_ms(&usb_ehci_sc[1].sc_bus, ms_list[i]);
		t1 = wlan_kernhist_now_us();
		printf("usb: usb_delay_ms(%u) -> %llu us\n", ms_list[i], t1 - t0);
	}
}

int usb_platform_init(void)
{
	int error;

	/* the config worker drains config_interrupts/config_defer hooks -
	 * usb.c defers its whole attach (taskq threads + root hub) there */
	config_deferred_run();

	/* D50 ordering: the domain SRST pulses reset shared USB blocks,
	 * so both domains run to completion before either HCD attaches -
	 * the USB3 socket-group domain first (its con9/con14 pulse is
	 * what makes the attach-time con14 pulse in the usb2phy1 domain
	 * bounce nothing), then the usb2phy1 domain. */
	usb_usb3_domain_init();
	usb_usb2phy1_domain_init();

	error = usb_xhci_attach(); /* the USB3 socket group (upper port) */
	if (error != 0) {
		return error;
	}
	error = usb_ehci_attach(1); /* the panel Type-A pair (CH334P hub) */
	if (error != 0) {
		return error;
	}
	return 0;
}
