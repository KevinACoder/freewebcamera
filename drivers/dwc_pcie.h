/*
 * @file   dwc_pcie.h
 * @brief  DesignWare PCIe root complex backend: internal driver API.
 *
 * Not an interface layer: include/pcie.h is what the rest of the project
 * sees (ARM_DRIVER_PCIE). This header exists for the two things that need
 * more than that interface offers:
 *
 *   - drivers/pci_msix.c, which programs an endpoint's MSI-X table and so
 *     needs the function's BAR and capability offsets - and the Requester
 *     ID, which is the ITS DeviceID;
 *   - drivers/dwc_nvme.c, which has to find its controller by class code
 *     before it can talk to it.
 *
 * PROVENANCE AND PORTING POLICY. Register sequences, iATU layout, the
 * firmware-inherited-link contract and the config-window retarget scheme are
 * ported from the author's own embox driver (embox/embox, BSD-2,
 * src/drivers/pci/pci_chip/pcie_dw.c, commit 5a938d03f2, board-verified on
 * this SoC 2026-09-07). The reference is the truth source: where it makes a
 * choice - slot 0 only, all-F on a non-existent function, the DSB between the
 * iATU enable store and its readback, the identity MEM window - the choice is
 * kept, not re-derived.
 *
 * Mechanical differences from the embox original, all noted at their site:
 *   - register/barrier primitives are this project's (regs.h);
 *   - per-controller coordinates come from a board table (rk3568_pcie.c)
 *     instead of module options;
 *   - the doorbell window is a board constant (BOARD_ITS_TRANSLATER) rather
 *     than an option: the GIC is one device in the SoC;
 *   - embox's generic PCI framework (bus scan, struct pci_slot_dev, the
 *     device list) is not vendored: this driver owns a small flat scan
 *     instead, which is the only thing the NVMe and MSI-X paths consume;
 *   - the config-window retarget is serialised with a mutex, which the
 *     original left to its single-threaded runlevel-3 probe.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_DWC_PCIE_H
#define FREEWEBCAMERA_DWC_PCIE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-controller coordinates. apb_base is the Rockchip client block whose
 * LTSSM status tells firmware from "never trained". */
struct dwc_pcie_plat {
	uintptr_t apb_base;
	uint64_t dbi_base;	/* DWC register file (also the root port config) */
	uint64_t cfg_base;	/* local config window, retargeted per access */
	uint64_t mem_base;	/* outbound memory window over the endpoint BARs */
	uint64_t mem_size;
	uint8_t bus_base;	/* root port bus; downstream is bus_base + 1 */
	uint16_t intx_irq;
};

#define DWC_PCIE_CTRL_COUNT 2

extern const struct dwc_pcie_plat dwc_pcie_plats[DWC_PCIE_CTRL_COUNT];

/* One enumerated function. BARs hold what the firmware assigned; this driver
 * never writes a BAR (sizing a BAR would have to overwrite it). */
struct dwc_pcie_dev {
	uint32_t busn;
	uint32_t slot;
	uint32_t func;
	uint32_t devfn;
	uint16_t vendor;
	uint16_t device;
	uint8_t baseclass;
	uint8_t subclass;
	uint8_t msix_cap;	/* config offset of the MSI-X capability, 0 = none */
	uint8_t msi_cap;	/* config offset of the MSI capability, 0 = none */
	uint32_t bar[6];
};

#define DWC_PCIE_DEV_MAX 8

/* Bring up both controllers: inherit the firmware link, program the iATU
 * windows (including the inbound doorbell window MSI needs) and scan the
 * downstream buses. Idempotent. */
void dwc_pcie_init(void);

/* Number of controllers whose link was inherited. */
uint32_t dwc_pcie_ctrl_count(void);

/* Config space access. size is 1, 2 or 4; `value` is right-justified (the
 * same convention the embox backend uses). Returns ARM_DRIVER_OK or a
 * negative ARM_DRIVER_ERROR*. */
int32_t dwc_pcie_cfg_read(uint32_t bus, uint32_t devfn, uint32_t where,
			  uint32_t size, uint32_t *value);
int32_t dwc_pcie_cfg_write(uint32_t bus, uint32_t devfn, uint32_t where,
			   uint32_t size, uint32_t value);

/* Flat device list filled by dwc_pcie_init(). */
uint32_t dwc_pcie_dev_count(void);
const struct dwc_pcie_dev *dwc_pcie_dev(uint32_t index);

/* The function's PCIe Requester ID (bus << 8 | slot << 3 | func), which is
 * the ITS DeviceID used for its message interrupts. */
uint32_t dwc_pcie_requester_id(const struct dwc_pcie_dev *dev);

/* Config space register offsets this driver's scan and the endpoint
 * MSI-X programmer (drivers/pci_msix.h) use. */
#define PCI_CFG_VENDOR_ID	0x00
#define PCI_CFG_COMMAND		0x04
#define PCI_CFG_REVISION	0x08
#define PCI_CFG_CLASS		0x0b	/* base class byte; subclass at 0x0a */
#define PCI_CFG_HEADER_TYPE	0x0e
#define PCI_CFG_CAP_PTR		0x34
#define PCI_CFG_BAR0		0x10

#define PCI_CMD_MEMORY		(1u << 1)
#define PCI_CMD_MASTER		(1u << 2)

#define PCI_CLASS_STORAGE	0x01
#define PCI_SUBCLASS_NVME	0x08

/* Capability ids the scan records. MSI-X is an endpoint capability, so its
 * table/entry layout lives with the programmer (drivers/pci_msix.h), not
 * with the host controller. */
#define PCI_CAP_ID_MSI		0x05
#define PCI_CAP_ID_MSIX		0x11

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_DWC_PCIE_H */