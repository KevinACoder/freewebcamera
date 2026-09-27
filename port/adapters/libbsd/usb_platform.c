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
#include "usb_domain.h"
#include "usb_platform.h"

/* the global DMA tag the compat bus.h documents (bsd_bus.c) */
extern bus_dma_tag_t wlan_bus_dma_tag;
extern void wlan_dma_pool_range(uintptr_t *base, size_t *len,
    uintptr_t *win_end);

/* The PD_PIPE/VBUS/usb2phy1 domain sequence lives in
 * port/adapters/usb/usb_domain.c: it is the same whichever host stack
 * drives the controllers, so the CherryUSB backend links it instead of a
 * second copy of these registers. */

/* ------------------------------------------------------------------
 * EHCI composition (the ehci_fdt.c role)
 */

static struct ehci_softc usb_ehci_sc[USBH_EHCI_NUM];
static struct device usb_ehci_dev[USBH_EHCI_NUM];
static bool s_usb_ehci_attached;

/* The usbus registry (see usb_platform.h): every bus handed to the usb
 * driver, in attach order.  Capacity covers the four buses this platform can
 * bring up - two EHCI roots plus the xHCI's two. */
#define USB_PLATFORM_MAX_BUSES	4U
static struct usbd_bus *s_usb_buses[USB_PLATFORM_MAX_BUSES];
static unsigned int s_usb_nbuses;

/* Shared with usb_xhci_platform.c, which attaches the xHCI's two buses and
 * must record them through the same registry (single writer per call site,
 * no locking: both run from usb_platform_init() before any consumer exists). */
void usb_platform_bus_record(struct usbd_bus *bus)
{
	if (bus != NULL && s_usb_nbuses < USB_PLATFORM_MAX_BUSES) {
		s_usb_buses[s_usb_nbuses++] = bus;
	}
}

unsigned int usb_platform_bus_count(void)
{
	return s_usb_nbuses;
}

struct usbd_bus *usb_platform_bus_at(unsigned int index)
{
	return (index < s_usb_nbuses) ? s_usb_buses[index] : NULL;
}

/* the CMSIS IRQ front hands no argument, and re-reading the INTID in the
 * handler is not an option: the trampoline has already ACKed this one and
 * ICC_IAR1 answers spurious until the EOI, so the dispatch would silently
 * drop every interrupt (the wedged-second-boot round).  One handler per
 * instance instead. */
static struct ehci_softc *usb_ehci_isr_sc[USBH_EHCI_NUM];
static uint32_t usb_ehci_irq(int id);
volatile unsigned usb_ehci_irq_count;
volatile unsigned usb_ehci_irq_last_sts;

static void usb_ehci_isr_instance(int id)
{
	struct ehci_softc *sc = usb_ehci_isr_sc[id];

	if (sc == NULL) {
		return;
	}
	usb_ehci_irq_count++;
	/* USBSTS is an operational register: an EREAD here reads the
	 * capability window (HCSPARAMS) and says nothing about the irq */
	usb_ehci_irq_last_sts = EOREAD4(sc, EHCI_USBSTS);
	(void) ehci_intr(sc);
}

static void usb_ehci_isr_0(void) { usb_ehci_isr_instance(0); }
static void usb_ehci_isr_1(void) { usb_ehci_isr_instance(1); }

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

	IRQ_SetHandler((IRQn_ID_t) irq,
	    (id == 0) ? usb_ehci_isr_0 : usb_ehci_isr_1);
	usb_ehci_isr_sc[id] = sc;
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
	usb_platform_bus_record(&sc->sc_bus);
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
	/* Both panel lines come up: each EHCI feeds its own onboard
	 * CH334P hub behind one Type-A connector.  The camera lives on
	 * the fd800000 group (uhub port4, native-fork evidence), urtwn
	 * on the fd880000 group - the u2phy1 domain is shared and its
	 * init is once-guarded, so only the attach order follows the
	 * device tree (fd800000 first). */
	error = usb_ehci_attach(1); /* the proven panel line first */
	if (error != 0) {
		return error;
	}
	printf("usb: ehci1 up, attaching ehci0 (camera group)\n");
	error = usb_ehci_attach(0);
	if (error != 0) {
		return error;
	}
	printf("usb: ehci0 up\n");
	return 0;
}
