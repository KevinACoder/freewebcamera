/*
 * @file
 * @brief Native PCI chipset glue: the DesignWare root-complex driver
 * (drivers/dwc_pcie.c, reached through the frozen include/pcie.h
 * interface) speaking the pci(9) vocabulary the NetBSD iwm import
 * expects.
 *
 * This replaces the abandoned fdt line wholesale: no NetBSD pci core,
 * no pciconf allocator, no ppb descent, no fdtbus/libfdt world.  What
 * remains is exactly the surface if_iwm.c reaches - pci_conf_read/
 * write, pci_mapreg_map, pci_get_capability and the pci_intr_alloc
 * family - implemented against the ARM_DRIVER_PCIE ops plus the
 * board's MSI domain (include/msi.h).
 *
 * Interrupt route: classic MSI first.  The endpoint's message write
 * rides the inbound-iATU doorbell window onto GITS_TRANSLATER - the
 * exact path the NVMe line validated end to end on this board.  INTx
 * is the fallback: its client-APB legacy aggregator was armed (SPI
 * 162 / INTID 194) in the fdt line but never exercised past arming,
 * and the +0x1c unmask write below comes from the same second-hand
 * note rather than a measured run - treat INTx delivery as unproven
 * until a board run says otherwise.
 *
 * The attach is a hand-rolled autoconf hop: config_found against the
 * "pci" iattr with a hand-filled pci_attach_args, iwm's own
 * iwm_match/iwm_attach run from its CFATTACH table, and iwm's
 * config_mountroot firmware hook runs synchronously (bsd_autoconf.c;
 * the blobs are embedded).
 *
 * @date 26.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <string.h>

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/bus.h>
#include <sys/intr.h>
#include <sys/workqueue.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>

#include "pcie.h"
#include "msi.h"
#include "irq_ctrl.h"
#include "board.h"

/* the DesignWare host backend (drivers/dwc_pcie.c) and its endpoint
 * MSI-X table programming (drivers/pci_msix.c) - the board-proven
 * message-interrupt route (NVMe line) */
extern ARM_DRIVER_PCIE Driver_PCIe;
#include "dwc_pcie.h"
#include "pci_msix.h"

/* the bus backend singletons (bsd_bus.c) */
extern bus_space_tag_t wlan_bus_space_tag;
extern bus_dma_tag_t wlan_bus_dma_tag;

/* the iwm adapter's post-attach registration (iwm_reg.c) */
void wlan_port_post_attach_iwm(device_t dev);

/* ------------------------------------------------------------------
 * board data: the controller instances, as the radio side sees them.
 * Kept in agreement with port/board/rk3568/rk3568_pcie.c - the driver
 * owns the coordinates; these are the three facts include/pcie.h does
 * not express and only the glue needs.
 */
static const struct pcie_glue_ctrl {
	uint32_t downstream_bus;	/* root bus is downstream_bus - 1 */
	uintptr_t apb_base;		/* client APB (legacy INTx block) */
	uint32_t intx_intid;		/* GIC INTID of the aggregated line */
} pcie_glue_ctrls[2] = {
	{ 1, 0xfe260000UL, 104 },	/* pcie2x1 (M.2) */
	{ 3, 0xfe280000UL, 194 },	/* pcie3x2 - the AC7260 slot */
};

#define PCIE_GLUE_CTRL_COUNT	2

/* downstream bus of the AC7260 slot (pcie3x2) */
#define PCIE_GLUE_RADIO_BUS	3u

/* client APB legacy interrupt mask register (RK TRM: +0x1c); the four
 * INTx lines aggregate onto one GIC line per controller */
#define PCIE_CLIENT_INT_MASK	0x1c

/* ------------------------------------------------------------------
 * interrupt cookie: one handler context (one radio, one vector)
 */

struct pcie_intr_cookie {
	int (*func)(void *);
	void *arg;
	uint32_t intid;
	uint32_t rid;
	uint8_t is_msi;
	void *pc_v;			/* chipset tag, for the MSI cap writes */
	pcitag_t tag;
	unsigned long count;
};

static struct pcie_intr_cookie pcie_cookie;
static volatile unsigned long pcie_spurious_count;
volatile unsigned long pcie_glue_irq_count;

/* the radio's BAR0, learned when the endpoint maps its BAR: the ISR
 * reads the cause registers there, which the log otherwise never shows */
static volatile uint32_t *pcie_radio_regs;

#define PCIE_RADIO_CSR_INT	0x008	/* read-clear cause register */
#define PCIE_RADIO_CSR_INT_MASK	0x00c
#define PCIE_RADIO_CSR_FH_INT	0x010	/* FH cause register */

/* the MSI vector pci_intr_alloc handed out, kept for establish/release */
static MSI_VECTOR pcie_msi_vector;

/* the intr ops (definitions below; the chipset tag references them) */
static const char *pcie_intr_string_impl(void *, pci_intr_handle_t, char *,
	size_t);
static void *pcie_intr_establish_impl(void *, pci_intr_handle_t, int,
	int (*)(void *), void *, const char *);
static void pcie_intr_disestablish_impl(void *, void *);
static int pcie_intr_map_stub(const struct pci_attach_args *,
	pci_intr_handle_t *);

static void
pcie_isr(void)
{
	pcie_glue_irq_count++;
	if (pcie_cookie.func == NULL) {
		pcie_spurious_count++;
		return;
	}
	pcie_cookie.count++;

	/* Forensics: name the interrupt cause at delivery.  Deliberately
	 * NON-destructive - CSR_INT is read-clear and consuming it here
	 * would steal the cause from the driver's softint (the first
	 * forensic round did exactly that); FH_INT_STATUS and INT_MASK
	 * are plain reads. */
	if (pcie_radio_regs != NULL && pcie_glue_irq_count <= 8ul) {
		board_log("pcie_glue: isr %lu fh=%08x mask=%08x\n",
		    pcie_glue_irq_count,
		    pcie_radio_regs[PCIE_RADIO_CSR_FH_INT / 4],
		    pcie_radio_regs[PCIE_RADIO_CSR_INT_MASK / 4]);
	}

	(void) pcie_cookie.func(pcie_cookie.arg);
}

/* ------------------------------------------------------------------
 * chipset tag ops: config space through the frozen interface
 */

static pcitag_t
pcie_make_tag(void *v, int bus, int dev, int func)
{
	(void) v;

	if (bus < 0 || bus > 255 || dev < 0 || dev > 31 || func < 0 ||
	    func > 7) {
		printf("pcie_glue: bad tag %d:%d.%d\n", bus, dev, func);
		return (pcitag_t) -1;
	}
	/* the arm32 encoding */
	return (pcitag_t) ((bus << 16) | (dev << 11) | (func << 8));
}

static void
pcie_decompose_tag(void *v, pcitag_t tag, int *bp, int *dp, int *fp)
{
	(void) v;

	if (bp != NULL) {
		*bp = (int) ((tag >> 16) & 0xffu);
	}
	if (dp != NULL) {
		*dp = (int) ((tag >> 11) & 0x1fu);
	}
	if (fp != NULL) {
		*fp = (int) ((tag >> 8) & 7u);
	}
}

static pcireg_t
pcie_conf_read(void *v, pcitag_t tag, int reg)
{
	PCIE_CONFIG_ACCESS acc;
	uint32_t value = 0xffffffffu;

	(void) v;

	acc.bus = (uint32_t) ((tag >> 16) & 0xffu);
	acc.device = (uint32_t) ((tag >> 11) & 0x1fu);
	acc.function = (uint32_t) ((tag >> 8) & 7u);
	acc.offset = (uint32_t) reg;
	acc.value = &value;

	/* a bus outside any inherited controller reads all-F, which is
	 * exactly what a dead function reads - the caller cannot tell
	 * the difference and does not need to */
	(void) Driver_PCIe.ConfigRead(&acc);
	return value;
}

static void
pcie_conf_write(void *v, pcitag_t tag, int reg, pcireg_t value)
{
	PCIE_CONFIG_ACCESS acc;

	(void) v;

	acc.bus = (uint32_t) ((tag >> 16) & 0xffu);
	acc.device = (uint32_t) ((tag >> 11) & 0x1fu);
	acc.function = (uint32_t) ((tag >> 8) & 7u);
	acc.offset = (uint32_t) reg;
	acc.value = &value;

	(void) Driver_PCIe.ConfigWrite(&acc);
}

static const struct arm32_pci_chipset pcie_glue_pc = {
	.pc_conf_v = __UNCONST(&pcie_glue_pc),
	.pc_make_tag = pcie_make_tag,
	.pc_decompose_tag = pcie_decompose_tag,
	.pc_conf_read = pcie_conf_read,
	.pc_conf_write = pcie_conf_write,
	.pc_intr_v = __UNCONST(&pcie_glue_pc),
	.pc_intr_map = pcie_intr_map_stub,
	.pc_intr_string = pcie_intr_string_impl,
	.pc_intr_establish = pcie_intr_establish_impl,
	.pc_intr_disestablish = pcie_intr_disestablish_impl,
};

/* ------------------------------------------------------------------
 * the endpoint's capability hunt (config walk, no framework)
 */

static int
pcie_find_cap(pci_chipset_tag_t pc, pcitag_t tag, uint8_t capid, int *offsetp)
{
	pcireg_t reg;
	int off;
	int guard;

	reg = pci_conf_read(pc, tag, PCI_CAPLISTPTR_REG);
	off = (int) (reg & 0xffu) & ~3;

	for (guard = 0; guard < 48 && off != 0; guard++) {
		pcireg_t hdr;

		if (off < 0x40 || off >= 0x100) {
			break;
		}
		hdr = pci_conf_read(pc, tag, off);
		if ((hdr & 0xffu) == capid) {
			if (offsetp != NULL) {
				*offsetp = off;
			}
			return 1;
		}
		off = (int) ((hdr >> 8) & 0xffu) & ~3;
	}
	return 0;
}

int
pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid,
	int *offsetp, pcireg_t *valuep)
{
	int off;

	if (pcie_find_cap(pc, tag, (uint8_t) capid, &off) == 0) {
		return 0;
	}
	if (offsetp != NULL) {
		*offsetp = off;
	}
	if (valuep != NULL) {
		*valuep = pci_conf_read(pc, tag, off + 4);
	}
	return 1;
}

/* ------------------------------------------------------------------
 * interrupts: MSI via the ITS doorbell, INTx as the fallback
 */

static int
pcie_msi_alloc_pa(const struct pci_attach_args *pa, pci_intr_handle_t *ihs,
	int nvec)
{
	const MSI_DOMAIN *domain;
	uint32_t rid;
	int msi_off, count;

	if (nvec != 1) {
		return EINVAL;	/* the radio wants exactly one vector */
	}
	if (pcie_find_cap(pa->pa_pc, pa->pa_tag, PCI_CAP_MSI, &msi_off) == 0) {
		return ENODEV;
	}
	domain = msi_domain_get();
	if (domain == NULL) {
		printf("pcie_glue: no MSI domain (ITS not up?)\n");
		return ENODEV;
	}

	rid = (pa->pa_bus << 8) | (pa->pa_device << 3) | pa->pa_function;
	count = domain->Allocate(rid, 1, &pcie_msi_vector);
	if (count < 1) {
		printf("pcie_glue: rid %04x got no MSI vectors\n", rid);
		return ENOMEM;
	}

	/* establish() programs the MSI capability from these */
	pcie_cookie.rid = rid;
	pcie_cookie.pc_v = __UNCONST(pa->pa_pc);
	pcie_cookie.tag = pa->pa_tag;

	/* handle shape (arm32): IRQ field = the LPI INTID to arm, the MSI
	 * flag marks the type */
	ihs[0] = (pci_intr_handle_t) pcie_msi_vector.intid |
	    ARM_PCI_INTR_MSI | ARM_PCI_INTR_MPSAFE;
	return 0;
}

static int
pcie_intx_alloc_pa(const struct pci_attach_args *pa, pci_intr_handle_t *ihs)
{
	size_t i;

	for (i = 0; i < PCIE_GLUE_CTRL_COUNT; i++) {
		if (pcie_glue_ctrls[i].downstream_bus == pa->pa_bus) {
			ihs[0] = (pci_intr_handle_t)
			    pcie_glue_ctrls[i].intx_intid;
			pcie_cookie.rid = (pa->pa_bus << 8) |
			    (pa->pa_device << 3) | pa->pa_function;
			pcie_cookie.pc_v = __UNCONST(pa->pa_pc);
			pcie_cookie.tag = pa->pa_tag;
			return 0;
		}
	}
	printf("pcie_glue: no INTx routing for bus %u\n", pa->pa_bus);
	return ENODEV;
}

/* the dwc device record behind a function, needed to program its MSI-X
 * table (pci_msix_arm addresses BARs and the cap offset itself) */
static const struct dwc_pcie_dev *
pcie_dwc_dev(const struct pci_attach_args *pa)
{
	uint32_t devfn = (pa->pa_device << 3) | pa->pa_function;
	uint32_t i;

	for (i = 0; i < dwc_pcie_dev_count(); i++) {
		const struct dwc_pcie_dev *dev = dwc_pcie_dev(i);

		if (dev != NULL && dev->busn == pa->pa_bus &&
		    dev->devfn == devfn) {
			return dev;
		}
	}
	return NULL;
}

/* the function whose MSI-X table this port armed (one radio) */
static const struct dwc_pcie_dev *pcie_msix_dev;

int
pci_msix_alloc(const struct pci_attach_args *pa, pci_intr_handle_t **ihsp,
	int *countp)
{
	const struct dwc_pcie_dev *dev = pcie_dwc_dev(pa);
	pci_intr_handle_t *ihs;
	MSI_VECTOR vec;
	int32_t count;

	if (dev == NULL || dev->msix_cap == 0) {
		return ENODEV;
	}

	/* the board-proven MSI-X route: program the endpoint's table with
	 * the vector the ITS domain hands out, raise ENABLE - the NVMe
	 * line this code is ported from delivered real interrupts on it */
	count = pci_msix_arm(dev, 1, &vec);
	if (count < 1) {
		return ENODEV;
	}

	ihs = kmem_zalloc(sizeof(*ihs), KM_SLEEP);
	if (ihs == NULL) {
		(void) pci_msix_disarm(dev);
		return ENOMEM;
	}
	ihs[0] = (pci_intr_handle_t) vec.intid | ARM_PCI_INTR_MSIX |
	    ARM_PCI_INTR_MPSAFE;
	pcie_msix_dev = dev;
	*ihsp = ihs;
	if (countp != NULL) {
		*countp = 1;
	}
	return 0;
}

int
pci_msix_alloc_exact(const struct pci_attach_args *pa,
	pci_intr_handle_t **ihsp, int nvec)
{
	if (nvec != 1) {
		return EINVAL;
	}
	return pci_msix_alloc(pa, ihsp, NULL);
}

int
pci_msix_alloc_map(const struct pci_attach_args *pa,
	pci_intr_handle_t **ihsp, u_int *indices, int nvec)
{
	(void) indices;
	return pci_msix_alloc_exact(pa, ihsp, nvec);
}

int
pci_intr_alloc(const struct pci_attach_args *pa, pci_intr_handle_t **ihsp,
	int *counts, pci_intr_type_t max_type)
{
	int intx_count, msi_count, msix_count;
	int error = EINVAL;

	/* NetBSD arm semantics (sys/arch/arm/pci/pci_msi_machdep.c): the
	 * type argument is a CEILING, and a NULL counts array - what iwm
	 * passes (`pci_intr_alloc(pa, &sc->sc_pihp, NULL, 0)`) - means
	 * "all classes, in the order MSI-X -> MSI -> INTx".  The fdt
	 * line's INTx-first (its D-10) was a workaround from before the
	 * ITS doorbell had been proven; the message route is the proven
	 * one on this board, and INTx is a level line that loses events
	 * when it deasserts between the signal and the IAR read (the
	 * first board run read 1023 exactly that way). */
	if (counts != NULL) {
		intx_count = msi_count = msix_count = 0;

		switch (max_type) {
		case PCI_INTR_TYPE_MSIX:
			msix_count = counts[PCI_INTR_TYPE_MSIX];
			/* FALLTHROUGH */
		case PCI_INTR_TYPE_MSI:
			msi_count = counts[PCI_INTR_TYPE_MSI];
			/* FALLTHROUGH */
		case PCI_INTR_TYPE_INTX:
			intx_count = counts[PCI_INTR_TYPE_INTX];
			if (intx_count > 1) {
				return EINVAL;
			}
			break;
		default:
			return EINVAL;
		}
		memset(counts, 0, sizeof(*counts) * PCI_INTR_TYPE_SIZE);
	} else {
		intx_count = msi_count = msix_count = 1;
	}

	if (msix_count > 0 &&
	    pci_msix_alloc_exact(pa, ihsp, msix_count) == 0) {
		if (counts != NULL) {
			counts[PCI_INTR_TYPE_MSIX] = msix_count;
		}
		return 0;
	}

	if (msi_count > 0 &&
	    pci_msi_alloc_exact(pa, ihsp, msi_count) == 0) {
		if (counts != NULL) {
			counts[PCI_INTR_TYPE_MSI] = msi_count;
		}
		return 0;
	}

	if (intx_count > 0 && pci_intx_alloc(pa, ihsp) == 0) {
		if (counts != NULL) {
			counts[PCI_INTR_TYPE_INTX] = intx_count;
		}
		return 0;
	}

	return error;
}

void
pci_intr_release(pci_chipset_tag_t pc, pci_intr_handle_t *ihs, int count)
{
	const MSI_DOMAIN *domain;

	(void) pc;

	if (ihs == NULL || count < 1) {
		return;
	}
	IRQ_Disable((IRQn_ID_t) (ihs[0] & ARM_PCI_INTR_IRQ));
	if ((ihs[0] & ARM_PCI_INTR_MSIX) != 0) {
		if (pcie_msix_dev != NULL) {
			(void) pci_msix_disarm(pcie_msix_dev);
			pcie_msix_dev = NULL;
		}
	} else if ((ihs[0] & ARM_PCI_INTR_MSI) != 0) {
		domain = msi_domain_get();
		if (domain != NULL) {
			domain->Free(pcie_cookie.rid);
		}
	}
	kmem_free(ihs, sizeof(*ihs));
}

pci_intr_type_t
pci_intr_type(pci_chipset_tag_t pc, pci_intr_handle_t ih)
{
	(void) pc;

	if ((ih & ARM_PCI_INTR_MSIX) != 0) {
		return PCI_INTR_TYPE_MSIX;
	}
	if ((ih & ARM_PCI_INTR_MSI) != 0) {
		return PCI_INTR_TYPE_MSI;
	}
	return PCI_INTR_TYPE_INTX;
}

int
pci_intx_alloc(const struct pci_attach_args *pa, pci_intr_handle_t **ihsp)
{
	pci_intr_handle_t *ihs = kmem_zalloc(sizeof(*ihs), KM_SLEEP);

	if (ihs == NULL) {
		return ENOMEM;
	}
	if (pcie_intx_alloc_pa(pa, ihs) != 0) {
		kmem_free(ihs, sizeof(*ihs));
		return ENODEV;
	}
	*ihsp = ihs;
	return 0;
}

int
pci_msi_alloc(const struct pci_attach_args *pa, pci_intr_handle_t **ihsp,
	int *countp)
{
	pci_intr_handle_t *ihs = kmem_zalloc(sizeof(*ihs), KM_SLEEP);

	if (ihs == NULL) {
		return ENOMEM;
	}
	if (pcie_msi_alloc_pa(pa, ihs, 1) != 0) {
		kmem_free(ihs, sizeof(*ihs));
		return ENODEV;
	}
	if (countp != NULL) {
		*countp = 1;
	}
	*ihsp = ihs;
	return 0;
}

int
pci_msi_alloc_exact(const struct pci_attach_args *pa,
	pci_intr_handle_t **ihsp, int nvec)
{
	if (nvec != 1) {
		return EINVAL;
	}
	return pci_msi_alloc(pa, ihsp, NULL);
}

static const char *
pcie_intr_string_impl(void *v, pci_intr_handle_t ih, char *buf, size_t len)
{
	uint32_t intid = (uint32_t) (ih & ARM_PCI_INTR_IRQ);

	(void) v;

	if ((ih & ARM_PCI_INTR_MSI) != 0) {
		snprintf(buf, len, "its msi intid %u (doorbell)", intid);
	} else {
		snprintf(buf, len, "gic spi %u (client apb intx)", intid - 32);
	}
	return buf;
}

/* the device-side half of arming.  MSI: program the capability (message
 * address = GITS_TRANSLATER, payload = the ITS event id) and enable it.
 * INTx: clear the client APB's legacy mask. */
static int
pcie_arm_device_side(const struct pcie_intr_cookie *ck)
{
	if (ck->is_msi) {
		pci_chipset_tag_t pc = (pci_chipset_tag_t) ck->pc_v;
		int msi_off;
		pcireg_t hdr;
		int addr_off, data_off;

		if (pcie_find_cap(pc, ck->tag, PCI_CAP_MSI, &msi_off) == 0) {
			return ENODEV;
		}
		/* the capability dword: [id(8) next(8) message-control(16)];
		 * the CTL_* macros are the upper-half field view */
		hdr = pci_conf_read(pc, ck->tag, msi_off);

		/* message address: the ITS doorbell the MSI domain handed
		 * out (GITS_TRANSLATER through the inbound iATU window);
		 * payload = the event id, which is the whole translation
		 * contract (DeviceID = rid, payload selects the LPI) */
		if ((hdr & PCI_MSI_CTL_64BIT_ADDR) != 0) {
			addr_off = msi_off + PCI_MSI_MADDR64_LO;
			data_off = msi_off + PCI_MSI_MDATA64;
			pci_conf_write(pc, ck->tag,
			    msi_off + PCI_MSI_MADDR64_HI,
			    (pcireg_t) (pcie_msi_vector.address >> 32));
		} else {
			addr_off = msi_off + PCI_MSI_MADDR;
			data_off = msi_off + PCI_MSI_MDATA;
		}
		pci_conf_write(pc, ck->tag, addr_off,
		    (pcireg_t) pcie_msi_vector.address);
		pci_conf_write(pc, ck->tag, data_off, pcie_msi_vector.data);

		/* enable last: endpoints sample the message pair when the
		 * enable rises (same discipline the MSI-X table follows) */
		pci_conf_write(pc, ck->tag, msi_off,
		    hdr | PCI_MSI_CTL_MSI_ENABLE);
		printf("pcie_glue: msi armed at %08llx data %u (intid %u)\n",
		    (unsigned long long) pcie_msi_vector.address,
		    pcie_msi_vector.data, ck->intid);
		return 0;
	}

	/* INTx: unmask the four legacy lines at the client APB.  The mask
	 * register is HIWORD-encoded (Linux pcie-dw-rockchip.c:
	 * HIWORD_DISABLE_BIT - the upper 16 bits are the write-enable for
	 * the lower half), so an unmasked write of 0 changed nothing: the
	 * lines stayed masked and the ucode download timed out waiting for
	 * an interrupt that could never arrive.  INTA..INTD are bits 0..3. */
	{
		size_t i;

		for (i = 0; i < PCIE_GLUE_CTRL_COUNT; i++) {
			if (pcie_glue_ctrls[i].intx_intid == ck->intid) {
				*(volatile uint32_t *)
				    (pcie_glue_ctrls[i].apb_base +
				    PCIE_CLIENT_INT_MASK) =
				    (0xfu << 16) | 0xfff0u;
				printf("pcie_glue: client apb %08lx intx"
				    " unmasked (intid %u)\n",
				    (unsigned long) pcie_glue_ctrls[i].apb_base,
				    ck->intid);
				return 0;
			}
		}
	}
	return ENODEV;
}

static void *
pcie_intr_establish_impl(void *v, pci_intr_handle_t ih, int ipl,
	int (*func)(void *), void *arg, const char *xname)
{
	uint32_t intid = (uint32_t) (ih & ARM_PCI_INTR_IRQ);

	(void) v; (void) ipl;

	if (pcie_cookie.func != NULL) {
		printf("pcie_glue: %s: interrupt already owned\n", xname);
		return NULL;
	}

	pcie_cookie.func = func;
	pcie_cookie.arg = arg;
	pcie_cookie.intid = intid;
	pcie_cookie.is_msi = ((ih & ARM_PCI_INTR_MSI) != 0);
	pcie_cookie.count = 0;

	/* CPU side first (the xHCI/INTx discipline: the handler is armed
	 * before the device can raise anything) */
	IRQ_SetHandler((IRQn_ID_t) intid, pcie_isr);
	IRQ_SetPriority((IRQn_ID_t) intid, BOARD_IRQ_PRIORITY_API_CALL_RAW);
	IRQ_Enable((IRQn_ID_t) intid);

	if ((ih & ARM_PCI_INTR_MSIX) != 0) {
		/* the endpoint table was programmed (and ENABLE raised) by
		 * pci_msix_arm at allocation time - nothing left to arm
		 * on the device side */
		printf("pcie_glue: msi-x vector armed (intid %u)\n", intid);
		return &pcie_cookie;
	}

	if (pcie_arm_device_side(&pcie_cookie) != 0) {
		IRQ_Disable((IRQn_ID_t) intid);
		pcie_cookie.func = NULL;
		return NULL;
	}

	return &pcie_cookie;
}

static void
pcie_intr_disestablish_impl(void *v, void *cookie)
{
	struct pcie_intr_cookie *ck = cookie;

	(void) v;

	if (ck != &pcie_cookie) {
		return;
	}
	IRQ_Disable((IRQn_ID_t) ck->intid);
	ck->func = NULL;
}

static int
pcie_intr_map_stub(const struct pci_attach_args *pa, pci_intr_handle_t *ihp)
{
	/* the legacy mapping: plain INTx (MSI goes through pci_intr_alloc) */
	return pcie_intx_alloc_pa(pa, ihp);
}

/* ------------------------------------------------------------------
 * BAR mapping: identity, no sizing, no BAR writes
 */

pcireg_t
pci_mapreg_type(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	pcireg_t rv;

	rv = pci_conf_read(pc, tag, reg);
	switch (rv & PCI_MAPREG_TYPE_MASK) {
	case PCI_MAPREG_TYPE_MEM:
		if ((rv & PCI_MAPREG_MEM_TYPE_MASK) ==
		    PCI_MAPREG_MEM_TYPE_64BIT) {
			return PCI_MAPREG_MEM_TYPE_64BIT;
		}
		return PCI_MAPREG_TYPE_MEM;
	case PCI_MAPREG_TYPE_IO:
		return PCI_MAPREG_TYPE_IO;
	default:
		return rv & (PCI_MAPREG_TYPE_MASK |
		    PCI_MAPREG_MEM_TYPE_MASK);
	}
}

int
pci_mapreg_map(const struct pci_attach_args *pa, int reg, pcireg_t bus_mask,
	int flags, bus_space_tag_t *iotp, bus_space_handle_t *iohp,
	bus_addr_t *basep, bus_size_t *sizep)
{
	pcireg_t rv;
	bus_addr_t base;

	(void) flags;

	rv = pci_conf_read(pa->pa_pc, pa->pa_tag, reg);
	if ((rv & bus_mask) != bus_mask) {
		printf("pcie_glue: BAR at %02x is not the expected type"
		    " (bar %08x mask %08x)\n", reg, rv, bus_mask);
		return EINVAL;
	}

	base = (bus_addr_t) (rv & PCI_MAPREG_MEM_ADDR_MASK);
	if ((rv & PCI_MAPREG_MEM_TYPE_MASK) == PCI_MAPREG_MEM_TYPE_64BIT) {
		base |= (bus_addr_t) pci_conf_read(pa->pa_pc, pa->pa_tag,
		    reg + 4) << 32;
	}

	/* identity mapping: the handle is the address.  The size is the
	 * remainder of the controller's MEM window - sizing the BAR would
	 * mean writing all-ones over the firmware assignment, which this
	 * port never does.  No consumer reads it on this driver. */
	if (iotp != NULL) {
		*iotp = wlan_bus_space_tag;
	}
	if (iohp != NULL) {
		*iohp = (bus_space_handle_t) base;
	}
	if (basep != NULL) {
		*basep = base;
	}
	if (sizep != NULL) {
		*sizep = 0x2000000;	/* 32 MiB: honest upper bound */
	}

	printf("pcie_glue: BAR%d mapped at %08llx\n",
	    PCI_MAPREG_NUM(reg), (unsigned long long) base);

	/* the radio's register file, for the ISR-side cause forensics
	 * (the AC7260 slot is the only endpoint this port maps) */
	if (reg == PCI_CFG_BAR0 && pa->pa_bus == PCIE_GLUE_RADIO_BUS) {
		pcie_radio_regs = (volatile uint32_t *)(uintptr_t) base;
	}
	return 0;
}

/* ------------------------------------------------------------------
 * pci_subr stand-ins: raw-id printer (no name database)
 */

void
pci_devinfo(pcireg_t id_reg, pcireg_t class_reg, int freq, char *buf,
	size_t len)
{
	(void) freq;

	snprintf(buf, len, "device %04x:%04x (class %06x)",
	    (unsigned) PCI_VENDOR(id_reg), (unsigned) PCI_PRODUCT(id_reg),
	    (unsigned) PCI_CLASS(class_reg));
}

void
pci_aprint_devinfo_fancy(const struct pci_attach_args *pa,
	const char *naive, const char *real, int construct)
{
	char devinfo[256];

	(void) construct;
	if (real == NULL) {
		pci_devinfo(pa->pa_id, pa->pa_class, 0, devinfo,
		    sizeof(devinfo));
		real = devinfo;
	}
	if (naive != NULL) {
		aprint_naive("%s\n", naive);
	}
	aprint_normal("%s\n", real);
}

void
pci_conf_print(pci_chipset_tag_t pc, pcitag_t tag,
	void (*printfn)(pci_chipset_tag_t, pcitag_t, const pcireg_t *))
{
	(void) pc; (void) tag; (void) printfn;
}

const struct pci_quirkdata *
pci_lookup_quirkdata(pci_vendor_id_t vendor, pci_product_id_t product)
{
	(void) vendor; (void) product;
	return NULL;
}

/* ------------------------------------------------------------------
 * the attach: hand-filled attach_args through the "pci" iattr
 */

static device_t pcie_pci_dev;	/* the "pci0" shell the endpoint hangs off */
static int pcie_glue_attached;

static device_t
pcie_shell_dev(const char *name)
{
	device_t dev = kmem_zalloc(sizeof(*dev), KM_SLEEP);

	if (dev != NULL) {
		strncpy(dev->dv_xname, name, sizeof(dev->dv_xname) - 1);
	}
	return dev;
}

int
pcie_glue_init(void)
{
	size_t ci;
	int attached = 0;

	if (pcie_glue_attached) {
		return 0;
	}
	pcie_glue_attached = 1;

	/* host up: inherits the firmware link (U-Boot preboot 'pci enum');
	 * no controller without a link contributes a bus */
	for (size_t i = 0; i < PCIE_GLUE_CTRL_COUNT; i++) {
		if (Driver_PCIe.Initialize((uint32_t) i, NULL) !=
		    ARM_DRIVER_OK) {
			printf("pcie_glue: pcie%zu: no firmware link,"
			    " bus empty\n", i);
		}
	}

	if (pcie_pci_dev == NULL) {
		pcie_pci_dev = pcie_shell_dev("pci0");
		if (pcie_pci_dev == NULL) {
			printf("pcie_glue: no memory for the pci shell\n");
			return -1;
		}
	}

	/* flat endpoint hunt: slot 0, all functions, per downstream bus.
	 * iwm_match does the claiming - everything else prints and
	 * passes. */
	for (ci = 0; ci < PCIE_GLUE_CTRL_COUNT; ci++) {
		const struct pcie_glue_ctrl *c = &pcie_glue_ctrls[ci];
		uint32_t bus = c->downstream_bus;

		/* LinkUp takes the controller INSTANCE (0/1), not the
		 * downstream bus number - the first board run skipped
		 * pcie1 because 3-1=2 indexed past the ctrl table */
		if (Driver_PCIe.LinkUp((uint32_t) ci) != ARM_DRIVER_OK) {
			continue;
		}
		for (uint32_t func = 0; func < 8u; func++) {
			struct pci_attach_args pa;
			pcitag_t tag;
			pcireg_t id;
			device_t dev;

			tag = pci_make_tag(&pcie_glue_pc, (int) bus, 0,
			    (int) func);
			id = pci_conf_read(&pcie_glue_pc, tag,
			    PCI_ID_REG);
			if (id == 0xffffffffu ||
			    PCI_VENDOR(id) == PCI_VENDOR_INVALID) {
				continue;
			}

			memset(&pa, 0, sizeof(pa));
			pa.pa_iot = wlan_bus_space_tag;
			pa.pa_memt = wlan_bus_space_tag;
			pa.pa_dmat = wlan_bus_dma_tag;
			pa.pa_dmat64 = wlan_bus_dma_tag;
			pa.pa_pc = __UNCONST(&pcie_glue_pc);
			pa.pa_flags = PCI_FLAGS_MEM_OKAY | PCI_FLAGS_MSI_OKAY;
			pa.pa_bus = bus;
			pa.pa_device = 0;
			pa.pa_function = func;
			pa.pa_tag = tag;
			pa.pa_id = id;
			pa.pa_class = pci_conf_read(&pcie_glue_pc, tag,
			    PCI_CLASS_REG);
			pa.pa_intrswiz = 0;
			pa.pa_intrtag = tag;
			pa.pa_intrpin = 1;
			pa.pa_intrline = 0;
			pa.pa_rawintrpin = 1;

			printf("pcie_glue: %02x:%02x.%u %04x:%04x\n",
			    bus, 0u, func, PCI_VENDOR(id), PCI_PRODUCT(id));

			dev = config_found(pcie_pci_dev, &pa, NULL,
			    CFARGS(.iattr = "pci"));
			if (dev != NULL) {
				printf("pcie_glue: %s claimed it\n",
				    device_xname(dev));
				wlan_port_post_attach_iwm(dev);
				attached++;
			}
		}
	}

	if (attached == 0) {
		printf("pcie_glue: no radio endpoint claimed\n");
		return -1;
	}
	return 0;
}

/* interrupt counters for the shell dumps */
unsigned long
pcie_glue_irq_counts(unsigned long *spurious)
{

	if (spurious != NULL) {
		*spurious = pcie_spurious_count;
	}
	return pcie_glue_irq_count;
}

/* ------------------------------------------------------------------
 * workqueue(9): one worker thread per workqueue, works drained in
 * enqueue order (iwm runs its newstate/softint callbacks here).
 * Carried over unchanged from the fdt line's backend.
 */

#include "cmsis_os2.h"

struct workqueue {
	workqueue_func_t	wq_func;
	void			*wq_arg;
	struct work		*wq_head, *wq_tail;
	volatile unsigned	wq_pending;
	void			*wq_thread;
};

extern void *wlan_port_thread_create(void (*run)(void *), void *arg);

static void workqueue_worker(void *arg) {
	struct workqueue *wq = arg;

	for (;;) {
		while (wq->wq_pending == 0) {
			osDelay(1);
		}
		wq->wq_pending = 0;
		while (wq->wq_head != NULL) {
			struct work *wk = wq->wq_head;

			wq->wq_head = wk->w_qnext;
			if (wq->wq_head == NULL) {
				wq->wq_tail = NULL;
			}
			wq->wq_func(wk, wq->wq_arg);
		}
	}
}

int
workqueue_create(struct workqueue **wqp, const char *name,
	workqueue_func_t func, void *arg, int pri, int ipl, int flags)
{
	struct workqueue *wq;

	(void) name; (void) pri; (void) ipl; (void) flags;

	wq = kmem_zalloc(sizeof(*wq), KM_SLEEP);
	if (wq == NULL) {
		return ENOMEM;
	}
	wq->wq_func = func;
	wq->wq_arg = arg;
	wq->wq_thread = wlan_port_thread_create(workqueue_worker, wq);
	if (wq->wq_thread == NULL) {
		kmem_free(wq, sizeof(*wq));
		return ENOMEM;
	}
	*wqp = wq;
	return 0;
}

void
workqueue_enqueue(struct workqueue *wq, struct work *wk, void *cpu)
{
	(void) cpu;

	wk->w_qnext = NULL;
	if (wq->wq_tail != NULL) {
		wq->wq_tail->w_qnext = wk;
	} else {
		wq->wq_head = wk;
	}
	wq->wq_tail = wk;
	wq->wq_pending = 1;
}

void
workqueue_destroy(struct workqueue *wq)
{
	kmem_free(wq, sizeof(*wq));
}
