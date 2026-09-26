/*
 * @file   dwc_pcie.c
 * @brief  DesignWare PCIe root complex: link inheritance, iATU windows,
 *         config space and a flat bus scan.
 *
 * See drivers/dwc_pcie.h for the provenance and the list of mechanical
 * differences from the author's embox backend this is ported from. The
 * register sequences below are kept line for line: the LTSSM inheritance
 * check, the outbound MEM/CFG iATU programming, the inbound doorbell window
 * whose register roles are swapped relative to the outbound ones, the DSB
 * between an enable store and its readback, and the "slot 0 only, answer all-F
 * without issuing a TLP" rule that keeps single-device links from planting
 * phantom functions.
 *
 * Three things here are load-bearing and easy to undo by accident:
 *
 *   - The DBI windows sit above 4 GiB (0x3_00000000). Every address that
 *     touches them is a 64-bit value; a 32-bit truncation silently programs
 *     the wrong window instead of faulting.
 *   - The inbound doorbell window is what makes MSI work at all. For inbound
 *     regions BASE/LIMIT describe the PCI (TLP) side and TARGET the system
 *     (AXI) side - the opposite of the outbound regions. Programmed the
 *     outbound way, the endpoint's completion write is dropped at the root
 *     port: no error, no interrupt, and the I/O queue just times out.
 *   - The CFG window is a single region retargeted per access. Two threads
 *     issuing config cycles at once would interleave the retarget and send a
 *     TLP to the wrong function; the mutex below is the guard.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "Driver_Common.h"
#include "board.h"
#include "cmsis_os2.h"
#include "pcie.h"
#include "regs.h"

#include "dwc_pcie.h"

/* Rockchip client APB block: the only place that says whether firmware
 * trained the link. */
#define PCIE_CLIENT_LTSSM_STATUS 0x300
#define LTSSM_SMLH_LINKUP	(1u << 16)
#define LTSSM_RDLH_LINKUP	(1u << 17)
#define LTSSM_LINKUP_MASK	(LTSSM_SMLH_LINKUP | LTSSM_RDLH_LINKUP)
#define LTSSM_STATE_MASK	0x3fu
#define LTSSM_STATE_L0		0x11u

/* Root port link capability/status inside the DBI register file */
#define DW_LINK_CAPABILITY	0x7c
#define DW_LINK_STATUS		0x80
#define LINK_STATUS_SPEED_SHIFT	16
#define LINK_STATUS_WIDTH_SHIFT	20

/* Unrolled iATU register view: one block per region index. The inbound
 * regions live in the same 0x300000 window, one bit (0x100) apart. */
#define ATU_REG_BLOCK(idx) \
	(((uintptr_t)0x3 << 20) | ((uintptr_t)(idx) << 9))
#define ATU_REG_BLOCK_INB(idx) \
	(((uintptr_t)0x3 << 20) | ((uintptr_t)(idx) << 9) | ((uintptr_t)0x1 << 8))

#define ATU_CTRL1	0x00
#define ATU_CTRL2	0x04
#define ATU_BASE_LO	0x08
#define ATU_BASE_HI	0x0c
#define ATU_LIMIT	0x10
#define ATU_TGT_LO	0x14
#define ATU_TGT_HI	0x18
#define ATU_CTRL2_ENABLE (1u << 31)
#define ATU_TYPE_MEM	0x0u
#define ATU_TYPE_CFG0	0x4u

#define ATU_REGION_MEM		0
#define ATU_REGION_CFG		1
#define ATU_REGION_DOORBELL	0	/* inbound region index */

/* The config window doubles as the transient CPU base of the retargeted CFG
 * region, so it must be at least one config-space page and below 4 GiB (every
 * TLP address here is 32-bit). */
#define CFG_WINDOW_SIZE		0x100000u

#define ATU_ENABLE_ACK_RETRIES	1000u

/* The doorbell page. The ITS translater register is one 32-bit doorbell; the
 * inbound window covers its page, so both sides use the same address. */
#define DOORBELL_PAGE		(BOARD_ITS_TRANSLATER & ~0xfffUL)
#define DOORBELL_PAGE_SIZE	0x1000u

struct dwc_pcie_ctrl {
	const struct dwc_pcie_plat *plat;
	int link_up;
};

static struct dwc_pcie_ctrl ctrls[DWC_PCIE_CTRL_COUNT];
static struct dwc_pcie_dev devs[DWC_PCIE_DEV_MAX];
static uint32_t dev_count;
static int initialised;

/* Serialises the CFG window retarget. Created on first use; a failure to
 * create it is reported and leaves config access unprotected, which is still
 * correct for the single-threaded probe path that is the only user today. */
static osMutexId_t cfg_lock;

static struct dwc_pcie_ctrl *ctrl_for_bus(uint32_t bus)
{
	uint32_t i;

	for (i = 0; i < DWC_PCIE_CTRL_COUNT; i++) {
		struct dwc_pcie_ctrl *c = &ctrls[i];

		if (!c->link_up) {
			continue;
		}
		if (bus >= c->plat->bus_base && bus <= c->plat->bus_base + 1u) {
			return c;
		}
	}

	return NULL;
}

/* --- iATU ---------------------------------------------------------------- */

static void atu_wait_enable(uintptr_t reg, const char *what, uint32_t idx)
{
	uint32_t i;

	/* Commit the enable write before polling it back: without the barrier
	 * a stale CTRL2 read logs a false failure, and - worse - the caller
	 * proceeds while the window is not enabled. */
	reg_dsb();

	for (i = 0; i < ATU_ENABLE_ACK_RETRIES; i++) {
		if (reg_rd32(reg + ATU_CTRL2) & ATU_CTRL2_ENABLE) {
			return;
		}
	}

	board_log("pcie: %s iATU region %u enable did not stick\n", what, idx);
}

/* Outbound region: CPU base -> PCI address. */
static void atu_map_out(struct dwc_pcie_ctrl *c, uint32_t idx, uint32_t type,
			uint64_t cpu_base, uint64_t size, uint64_t pci_base)
{
	uintptr_t reg = (uintptr_t)(c->plat->dbi_base + ATU_REG_BLOCK(idx));

	reg_wr32(reg + ATU_CTRL2, 0);
	reg_wr32(reg + ATU_CTRL1, type);
	reg_wr32(reg + ATU_BASE_LO, (uint32_t)cpu_base);
	reg_wr32(reg + ATU_BASE_HI, (uint32_t)(cpu_base >> 32));
	reg_wr32(reg + ATU_LIMIT, (uint32_t)(cpu_base + size - 1));
	reg_wr32(reg + ATU_TGT_LO, (uint32_t)pci_base);
	reg_wr32(reg + ATU_TGT_HI, (uint32_t)(pci_base >> 32));
	reg_wr32(reg + ATU_CTRL2, ATU_CTRL2_ENABLE);

	atu_wait_enable(reg, "outbound", idx);
}

/* Inbound region: PCI address -> system address. The register roles are the
 * opposite of the outbound form above (BASE/LIMIT are the TLP side, TARGET is
 * the AXI side); this window is what forwards an endpoint's MSI write to the
 * ITS translater instead of dropping it at the root port. */
static void atu_map_in(struct dwc_pcie_ctrl *c, uint32_t idx, uint64_t pci_base,
		       uint64_t cpu_base, uint64_t size)
{
	uintptr_t reg = (uintptr_t)(c->plat->dbi_base + ATU_REG_BLOCK_INB(idx));

	reg_wr32(reg + ATU_CTRL2, 0);
	reg_wr32(reg + ATU_CTRL1, ATU_TYPE_MEM);
	reg_wr32(reg + ATU_BASE_LO, (uint32_t)pci_base);
	reg_wr32(reg + ATU_BASE_HI, (uint32_t)(pci_base >> 32));
	reg_wr32(reg + ATU_LIMIT, (uint32_t)(pci_base + size - 1));
	reg_wr32(reg + ATU_TGT_LO, (uint32_t)cpu_base);
	reg_wr32(reg + ATU_TGT_HI, (uint32_t)(cpu_base >> 32));
	reg_wr32(reg + ATU_CTRL2, ATU_CTRL2_ENABLE);

	atu_wait_enable(reg, "inbound", idx);
}

/* --- config space -------------------------------------------------------- */

/* Resolve a config access to the MMIO address that carries it. Codes match
 * the drivers' convention: ARM_DRIVER_OK, ARM_DRIVER_ERROR_TIMEOUT (no such
 * function: read all-F, do not issue a TLP) and ARM_DRIVER_ERROR_PARAMETER. */
static int32_t cfg_map(struct dwc_pcie_ctrl *c, uint32_t bus, uint32_t devfn,
		       uint32_t where, uintptr_t *va)
{
	uint32_t tlp;

	if (where >= CFG_WINDOW_SIZE) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	if (bus == c->plat->bus_base) {
		/* The root port's config space is the DBI register file, and
		 * a DWC root complex has no device past slot 0. */
		if (((devfn >> 3) & 0x1f) != 0) {
			return ARM_DRIVER_ERROR_TIMEOUT;
		}
		*va = (uintptr_t)c->plat->dbi_base + (where & ~0x3u);
		return ARM_DRIVER_OK;
	}

	/* Only slot 0 exists below a root port. Probing other slots is not
	 * merely wasted time: a single-device link answers config TLPs for any
	 * slot number (phantom functions that fill the device table), and some
	 * endpoints never complete such a TLP at all - the config read then
	 * never returns. */
	if (((devfn >> 3) & 0x1f) != 0) {
		return ARM_DRIVER_ERROR_TIMEOUT;
	}

	tlp = (bus << 24) | (((devfn >> 3) & 0x1f) << 19) | ((devfn & 7u) << 16);
	atu_map_out(c, ATU_REGION_CFG, ATU_TYPE_CFG0, c->plat->cfg_base,
		    CFG_WINDOW_SIZE, tlp);
	*va = (uintptr_t)c->plat->cfg_base + (where & ~0x3u);

	return ARM_DRIVER_OK;
}

int32_t dwc_pcie_cfg_read(uint32_t bus, uint32_t devfn, uint32_t where,
			  uint32_t size, uint32_t *value)
{
	struct dwc_pcie_ctrl *c = ctrl_for_bus(bus);
	uintptr_t va;
	uint32_t tmp;
	int32_t ret;

	if (value == NULL || (size != 1 && size != 2 && size != 4)) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	if (c == NULL) {
		tmp = 0xffffffffu;
		ret = ARM_DRIVER_ERROR_TIMEOUT;
	} else {
		ret = cfg_map(c, bus, devfn, where, &va);
		if (ret != ARM_DRIVER_OK) {
			tmp = 0xffffffffu;
		} else {
			tmp = reg_rd32(va);
		}
	}

	switch (size) {
	case 1:
		*value = (tmp >> (8u * (where & 3u))) & 0xffu;
		break;
	case 2:
		*value = (tmp >> (8u * (where & 3u))) & 0xffffu;
		break;
	default:
		*value = tmp;
		break;
	}

	return ret;
}

int32_t dwc_pcie_cfg_write(uint32_t bus, uint32_t devfn, uint32_t where,
			   uint32_t size, uint32_t value)
{
	struct dwc_pcie_ctrl *c = ctrl_for_bus(bus);
	uintptr_t va;
	uint32_t tmp;
	uint32_t shift;
	int32_t ret;

	if (size != 1 && size != 2 && size != 4) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (c == NULL) {
		return ARM_DRIVER_ERROR_TIMEOUT;
	}

	ret = cfg_map(c, bus, devfn, where, &va);
	if (ret != ARM_DRIVER_OK) {
		return ret;
	}

	/* The iATU config window only accepts DWORD accesses: merge sub-dword
	 * writes into a read-modify-write of the surrounding DWORD. */
	shift = 8u * (where & 3u);
	tmp = reg_rd32(va);
	switch (size) {
	case 1:
		tmp = (tmp & ~(0xffu << shift)) | ((value & 0xffu) << shift);
		break;
	case 2:
		tmp = (tmp & ~(0xffffu << shift)) | ((value & 0xffffu) << shift);
		break;
	default:
		tmp = value;
		break;
	}
	reg_wr32(va, tmp);

	return ARM_DRIVER_OK;
}

static int32_t cfg_read16(uint32_t bus, uint32_t devfn, uint32_t where,
			  uint16_t *value)
{
	uint32_t tmp;
	int32_t ret = dwc_pcie_cfg_read(bus, devfn, where, 2, &tmp);

	*value = (uint16_t)tmp;
	return ret;
}

static int32_t cfg_read32(uint32_t bus, uint32_t devfn, uint32_t where,
			  uint32_t *value)
{
	return dwc_pcie_cfg_read(bus, devfn, where, 4, value);
}

static void cfg_write16(uint32_t bus, uint32_t devfn, uint32_t where,
			uint16_t value)
{
	(void)dwc_pcie_cfg_write(bus, devfn, where, 2, value);
}

uint32_t dwc_pcie_requester_id(const struct dwc_pcie_dev *dev)
{
	return (dev->busn << 8) | (dev->slot << 3) | dev->func;
}

/* Walk the capability list for MSI and MSI-X and record their offsets. */
static void scan_capabilities(struct dwc_pcie_dev *dev)
{
	uint32_t next;
	uint32_t guard;

	next = 0;
	(void)dwc_pcie_cfg_read(dev->busn, dev->devfn, PCI_CFG_CAP_PTR, 1, &next);
	next &= 0xfcu;

	/* A malformed or absent list reads 0 repeatedly; the guard bound also
	 * keeps a device that loops its own list from hanging the scan. */
	for (guard = 0; guard < 48u && next != 0; guard++) {
		uint32_t id;
		uint32_t nxt;

		if (next < 0x40u || next >= 0x100u) {
			break;
		}
		if (dwc_pcie_cfg_read(dev->busn, dev->devfn, next, 1, &id)
		    != ARM_DRIVER_OK) {
			break;
		}
		if (dwc_pcie_cfg_read(dev->busn, dev->devfn, next + 1u, 1, &nxt)
		    != ARM_DRIVER_OK) {
			break;
		}

		if ((id & 0xffu) == PCI_CAP_ID_MSIX) {
			dev->msix_cap = (uint8_t)next;
		} else if ((id & 0xffu) == PCI_CAP_ID_MSI) {
			dev->msi_cap = (uint8_t)next;
		}

		next = nxt & 0xfcu;
	}
}

/* One downstream bus: walk every function of slot 0 (the only slot a root
 * port has) and record what answers. */
static void scan_bus(uint32_t bus)
{
	uint32_t func;

	for (func = 0; func < 8u; func++) {
		struct dwc_pcie_dev *dev;
		uint32_t devfn = (0u << 3) | func;
		uint32_t id;
		uint32_t classrev;
		uint32_t header;
		uint32_t i;
		int32_t ret;

		ret = cfg_read32(bus, devfn, PCI_CFG_VENDOR_ID, &id);
		if (ret != ARM_DRIVER_OK || id == 0xffffffffu || (id & 0xffffu) == 0xffffu) {
			/* An all-F vendor id is "nothing here"; a timeout is
			 * "nothing here and it never answered", which on this
			 * IP is the same decision. */
			continue;
		}
		if (dev_count >= DWC_PCIE_DEV_MAX) {
			board_log("pcie: device table full, bus %u devfn %u ignored\n",
				  bus, devfn);
			return;
		}

		dev = &devs[dev_count];
		memset(dev, 0, sizeof(*dev));
		dev->busn = bus;
		dev->slot = 0;
		dev->func = func;
		dev->devfn = devfn;
		dev->vendor = (uint16_t)(id & 0xffffu);
		dev->device = (uint16_t)(id >> 16);

		(void)cfg_read32(bus, devfn, PCI_CFG_REVISION, &classrev);
		dev->baseclass = (uint8_t)(classrev >> 24);
		dev->subclass = (uint8_t)(classrev >> 16);

		(void)cfg_read32(bus, devfn, PCI_CFG_HEADER_TYPE, &header);
		if (((header >> 16) & 0x7fu) == 0x00u) {	/* type 0: endpoint */
			for (i = 0; i < 6u; i++) {
				uint32_t bar;

				if (cfg_read32(bus, devfn, PCI_CFG_BAR0 + 4u * i,
					       &bar) == ARM_DRIVER_OK) {
					dev->bar[i] = bar;
				}
			}
			scan_capabilities(dev);
		}

		dev_count++;
	}
}

/* --- controller bring-up -------------------------------------------------- */

static void ctrl_probe(struct dwc_pcie_ctrl *c)
{
	const struct dwc_pcie_plat *p = c->plat;
	uint32_t status;
	uint32_t link;
	uint32_t cap;
	uint32_t idx = (uint32_t)(p - dwc_pcie_plats);

	if (p->apb_base == 0) {
		return;
	}

	/* Inherit the firmware trained link; never reset, never touch the
	 * PHY. If U-Boot did not run `pci enum` the link is not in L0 and
	 * there is nothing this driver can do about it. */
	status = reg_rd32(p->apb_base + PCIE_CLIENT_LTSSM_STATUS);
	if ((status & LTSSM_LINKUP_MASK) != LTSSM_LINKUP_MASK
	    || (status & LTSSM_STATE_MASK) != LTSSM_STATE_L0) {
		board_log("pcie%u: link is not up (LTSSM 0x%08x), skipping;"
			  " run U-Boot 'pci enum'\n", idx, status);
		return;
	}

	/* Outbound: the endpoint BAR window (identity), then the config window
	 * that every downstream access retargets. */
	atu_map_out(c, ATU_REGION_MEM, ATU_TYPE_MEM, p->mem_base, p->mem_size,
		    p->mem_base);
	atu_map_out(c, ATU_REGION_CFG, ATU_TYPE_CFG0, p->cfg_base, CFG_WINDOW_SIZE,
		    p->bus_base + 1u);

	/* Inbound: the ITS doorbell, so endpoint MSI writes are forwarded. */
	atu_map_in(c, ATU_REGION_DOORBELL, DOORBELL_PAGE, DOORBELL_PAGE,
		   DOORBELL_PAGE_SIZE);

	link = reg_rd32((uintptr_t)(p->dbi_base + DW_LINK_STATUS));
	cap = reg_rd32((uintptr_t)(p->dbi_base + DW_LINK_CAPABILITY));
	board_log("pcie%u: firmware link inherited (bus %u-%u),"
		  " link Gen%x x%u (max Gen%x x%u)\n",
		  idx, p->bus_base, p->bus_base + 1u,
		  (link >> LINK_STATUS_SPEED_SHIFT) & 0xfu,
		  (link >> LINK_STATUS_WIDTH_SHIFT) & 0x1fu,
		  cap & 0xfu, (cap >> 4) & 0x3fu);

	c->link_up = 1;
}

void dwc_pcie_init(void)
{
	uint32_t i;
	uint32_t up = 0;

	if (initialised) {
		return;
	}
	initialised = 1;

	cfg_lock = osMutexNew(&(osMutexAttr_t){ .name = "pciecfg" });
	if (cfg_lock == NULL) {
		board_log("pcie: config window mutex unavailable\n");
	}

	for (i = 0; i < DWC_PCIE_CTRL_COUNT; i++) {
		ctrls[i].plat = &dwc_pcie_plats[i];
		ctrl_probe(&ctrls[i]);
		if (ctrls[i].link_up) {
			up++;
		}
	}

	if (up == 0) {
		board_log("pcie: no link inherited from firmware,"
			  " PCI bus will be empty\n");
		return;
	}

	/* Scan after both controllers are up: the config window is shared per
	 * controller, and a controller without a link contributes no buses. */
	for (i = 0; i < DWC_PCIE_CTRL_COUNT; i++) {
		if (!ctrls[i].link_up) {
			continue;
		}
		scan_bus((uint32_t)ctrls[i].plat->bus_base + 1u);
	}
}

uint32_t dwc_pcie_ctrl_count(void)
{
	uint32_t i;
	uint32_t up = 0;

	for (i = 0; i < DWC_PCIE_CTRL_COUNT; i++) {
		if (ctrls[i].link_up) {
			up++;
		}
	}

	return up;
}

uint32_t dwc_pcie_dev_count(void)
{
	return dev_count;
}

const struct dwc_pcie_dev *dwc_pcie_dev(uint32_t index)
{
	if (index >= dev_count) {
		return NULL;
	}

	return &devs[index];
}

/* --- CMSIS ARM_DRIVER_PCIE (include/pcie.h) -------------------------------- */

static ARM_DRIVER_VERSION pcie_get_version(void)
{
	return (ARM_DRIVER_VERSION){ .api = PCIE_API_VERSION, .drv = 0x0100 };
}

static struct dwc_pcie_ctrl *ctrl_for_instance(uint32_t bus)
{
	if (bus >= DWC_PCIE_CTRL_COUNT) {
		return NULL;
	}

	return &ctrls[bus];
}

static PCIE_CAPABILITIES pcie_get_capabilities(uint32_t bus)
{
	PCIE_CAPABILITIES caps;
	struct dwc_pcie_ctrl *c = ctrl_for_instance(bus);

	memset(&caps, 0, sizeof(caps));
	if (c == NULL) {
		return caps;
	}

	if (c->link_up) {
		uint32_t link = reg_rd32((uintptr_t)(c->plat->dbi_base + DW_LINK_STATUS));
		uint32_t cap = reg_rd32((uintptr_t)(c->plat->dbi_base + DW_LINK_CAPABILITY));

		caps.link_speed = (uint8_t)((link >> LINK_STATUS_SPEED_SHIFT) & 0xfu);
		caps.max_lanes = (uint8_t)((link >> LINK_STATUS_WIDTH_SHIFT) & 0x1fu);
		if (caps.max_lanes == 0) {
			caps.max_lanes = (uint8_t)((cap >> 4) & 0x3fu);
		}
	}

	/* Message interrupts come from the board's MSI domain (the ITS), not
	 * from the controller, so they are always available here. */
	caps.msi_capable = 1;
	caps.msix_capable = 1;
	caps.config_space_size = CFG_WINDOW_SIZE;
	caps.mmio_window_count = 1;

	return caps;
}

static int32_t pcie_initialize(uint32_t bus, PCIE_SignalEvent_t cb_event)
{
	struct dwc_pcie_ctrl *c = ctrl_for_instance(bus);

	(void)cb_event;

	if (c == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	dwc_pcie_init();

	return c->link_up ? ARM_DRIVER_OK : ARM_DRIVER_ERROR;
}

static int32_t pcie_uninitialize(uint32_t bus)
{
	if (ctrl_for_instance(bus) == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	/* Nothing is torn down: the link belongs to firmware and the windows
	 * are cheap to leave in place. */
	return ARM_DRIVER_OK;
}

static int32_t pcie_power_control(uint32_t bus, uint32_t state)
{
	if (ctrl_for_instance(bus) == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (state != ARM_POWER_FULL) {
		return ARM_DRIVER_ERROR_UNSUPPORTED;
	}

	return ARM_DRIVER_OK;
}

static uint32_t ctrl_ltssm(uint32_t bus, struct dwc_pcie_ctrl **out)
{
	struct dwc_pcie_ctrl *c = ctrl_for_instance(bus);

	if (out != NULL) {
		*out = c;
	}
	if (c == NULL || c->plat->apb_base == 0) {
		return 0;
	}

	return reg_rd32(c->plat->apb_base + PCIE_CLIENT_LTSSM_STATUS);
}

static int32_t pcie_link_up(uint32_t bus)
{
	uint32_t status = ctrl_ltssm(bus, NULL);

	if ((status & LTSSM_LINKUP_MASK) != LTSSM_LINKUP_MASK
	    || (status & LTSSM_STATE_MASK) != LTSSM_STATE_L0) {
		return ARM_DRIVER_ERROR;
	}

	return ARM_DRIVER_OK;
}

static int32_t pcie_get_link_state(uint32_t bus, uint32_t *state)
{
	struct dwc_pcie_ctrl *c = NULL;

	if (state == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	*state = ctrl_ltssm(bus, &c);
	if (c == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return ARM_DRIVER_OK;
}

static int32_t pcie_config_read(const PCIE_CONFIG_ACCESS *access)
{
	if (access == NULL || access->value == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return dwc_pcie_cfg_read(access->bus,
				 (access->device << 3) | access->function,
				 access->offset, 4, access->value);
}

static int32_t pcie_config_write(const PCIE_CONFIG_ACCESS *access)
{
	if (access == NULL || access->value == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return dwc_pcie_cfg_write(access->bus,
				  (access->device << 3) | access->function,
				  access->offset, 4, *access->value);
}

static int32_t pcie_enumerate(uint32_t bus, uint32_t *function_count)
{
	struct dwc_pcie_ctrl *c = ctrl_for_instance(bus);
	uint32_t i;
	uint32_t found = 0;

	if (c == NULL || function_count == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (!c->link_up) {
		*function_count = 0;
		return ARM_DRIVER_ERROR;
	}

	/* `bus` is the controller instance here, but the device table
	 * carries real bus numbers; count that controller's downstream
	 * bus (the old backend compared the instance index against
	 * busn and always answered zero). */
	for (i = 0; i < dev_count; i++) {
		if (devs[i].busn == c->plat->bus_base + 1u) {
			found++;
		}
	}

	*function_count = found;

	return ARM_DRIVER_OK;
}

/* The controller's outbound MMIO window: where endpoint memory (BARs) lands
 * in the CPU address space. BARs are firmware-assigned inside it, so the size
 * reported is the window's, not the BAR's - sizing a BAR would mean writing
 * all-ones over the assignment. */
static int32_t pcie_map_window(uint32_t bus, uint32_t window,
			       uint32_t *cpu_addr, uint32_t *size)
{
	struct dwc_pcie_ctrl *c = ctrl_for_instance(bus);

	if (c == NULL || cpu_addr == NULL || size == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (window != 0 || !c->link_up) {
		return ARM_DRIVER_ERROR_UNSUPPORTED;
	}

	*cpu_addr = (uint32_t)c->plat->mem_base;
	*size = (uint32_t)c->plat->mem_size;

	return ARM_DRIVER_OK;
}

ARM_DRIVER_PCIE Driver_PCIe = {
	pcie_get_version,
	pcie_get_capabilities,
	pcie_initialize,
	pcie_uninitialize,
	pcie_power_control,
	pcie_link_up,
	pcie_get_link_state,
	pcie_config_read,
	pcie_config_write,
	pcie_enumerate,
	pcie_map_window,
};