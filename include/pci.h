/*
 * @file   pci.h
 * @brief  PCI function record + config-access backend - the standard layer
 *         between the controller drivers and the endpoint helpers.
 *
 * CMSIS-Driver covers peripherals, not the PCI bus itself. This header is
 * the thin, transport-agnostic half of that gap (include/pcie.h holds the
 * host-controller ops): everything that speaks "PCI function" - the MSI-X
 * table programmer (drivers/pci_msix.c), a future config-space helper, an
 * INTx router - takes one of these records plus a config backend and knows
 * nothing about which controller enumerated the function.
 *
 * CONTRACT:
 *  - `struct pci_func` is plain data, filled in by the enumerating driver.
 *  - `struct pci_cfg_backend` is the only way a generic consumer touches
 *    config space; it maps to whichever controller owns the bus segment.
 *  - `pci_func_requester_id()` is the PCIe Requester ID (bus << 8 | devfn),
 *    which message-interrupt domains (ITS DeviceID) key off.
 */

#ifndef FREEWEBCAMERA_PCI_H
#define FREEWEBCAMERA_PCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One enumerated PCI function. BARs hold what firmware assigned; the
 * generic layer never writes a BAR. */
struct pci_func {
	uint32_t busn;
	uint32_t slot;
	uint32_t func;
	uint32_t devfn;		/* slot << 3 | func */
	uint16_t vendor;
	uint16_t device;
	uint8_t baseclass;
	uint8_t subclass;
	uint8_t msix_cap;	/* config offset of the MSI-X capability, 0 = none */
	uint8_t msi_cap;	/* config offset of the MSI capability, 0 = none */
	uint32_t bar[6];
};

/* Config-space access, 32-bit aligned, right-justified values; sizes 1, 2
 * or 4. Implemented by whichever controller driver enumerated the bus. */
typedef int32_t (*pci_cfg_read_t)(uint32_t busn, uint32_t devfn,
				  uint32_t offset, uint32_t size,
				  uint32_t *value);
typedef int32_t (*pci_cfg_write_t)(uint32_t busn, uint32_t devfn,
				   uint32_t offset, uint32_t size,
				   uint32_t value);

struct pci_cfg_backend {
	pci_cfg_read_t read;
	pci_cfg_write_t write;
};

/* PCIe Requester ID for a function: bus << 8 | devfn. */
static inline uint16_t pci_func_requester_id(const struct pci_func *pf)
{
	return (uint16_t) ((pf->busn << 8) | pf->devfn);
}

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_PCI_H */
