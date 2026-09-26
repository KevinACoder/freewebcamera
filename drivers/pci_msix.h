/*
 * @file   pci_msix.h
 * @brief  MSI-X table programming for a PCIe endpoint function.
 *
 * MSI-X is an endpoint capability (PCIe spec 7.7.2), not a property of the
 * host controller IP: this file knows how to program a function's table and
 * raise its ENABLE, and nothing about who enumerates it. The function record
 * it consumes (struct dwc_pcie_dev) is the host driver's scan output - BARs
 * and the capability offset are PCI facts that driver collected.
 *
 * Internal to drivers/: an endpoint driver (dwc_nvme.c today, an 802.11 one
 * later) calls these two functions and gets back armed vectors; nothing above
 * the driver layer sees MSI-X at all. The vectors themselves come from the
 * board's MSI domain (include/msi.h) - the ITS side lives in port/aarch64/.
 *
 * PROVENANCE AND PORTING POLICY. This is the behaviour of the author's embox
 * MSI work without the framework underneath it: the ordering discipline comes
 * from src/drivers/pci/msi/pci_msi.c (the 141 lines the author wrote on top of
 * upstream's file, commit da44e283fd) and src/drivers/interrupt/gic/gicv3_its_msi.c
 * (commit da44e283fd, author's own, carried over whole into port/aarch64/).
 * embox's version rides on irq_domain, objalloc pools, dlist and its PCI
 * framework; none of that is vendored here, so what remains is the part that
 * touches hardware, kept step for step:
 *
 *   - mask the entry, order it, write the address/data triple, order it,
 *     unmask - a posted message write observed while the vector is unmasked
 *     can arm a half-written entry (PCIe spec 7.7.2.1);
 *   - hold the function masked (MASKALL) while the entries are programmed,
 *     unmask every entry, and only then raise ENABLE with MASKALL cleared in
 *     a single config write: endpoints sample the table as ENABLE rises and
 *     one that captures a masked state never re-samples;
 *   - the payload written into an entry is the ITS event id, which is also
 *     the entry index - the mapping the ITS glue was written against.
 *
 * @author zhugengyu
 * @date   26.09.2026
 */

#ifndef FREEWEBCAMERA_PCI_MSIX_H
#define FREEWEBCAMERA_PCI_MSIX_H

#include <stdint.h>

#include "msi.h"

#include "dwc_pcie.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- MSI-X capability layout, relative to the capability offset ---------- */

#define PCI_MSIX_FLAGS		0x02	/* 16 bit: ENABLE, MASKALL, table size */
#define PCI_MSIX_TABLE		0x04	/* 32 bit: BIR [2:0], offset [31:3] */
#define PCI_MSIX_FLAGS_ENABLE	(1u << 15)
#define PCI_MSIX_FLAGS_MASKALL	(1u << 14)
#define PCI_MSIX_FLAGS_QSIZE	0x07ffu
#define PCI_MSIX_TABLE_BIR	0x00000007u
#define PCI_MSIX_TABLE_OFFSET	0xfffffff8u

/* --- one MSI-X table entry, 16 bytes ------------------------------------ */

#define PCI_MSIX_ENTRY_SIZE	16
#define PCI_MSIX_ENTRY_LOWER_ADDR	0x00
#define PCI_MSIX_ENTRY_UPPER_ADDR	0x04
#define PCI_MSIX_ENTRY_DATA		0x08
#define PCI_MSIX_ENTRY_VECTOR_CTRL	0x0c
#define PCI_MSIX_ENTRY_CTRL_MASKBIT	0x00000001u

/* How many vectors one function may be given. The ITS hands out 48 LPIs in
 * total and the doorbell's event id must stay inside the device's ITT (64
 * entries), so a handful is the honest cap for a storage endpoint. */
#define PCI_MSIX_VECTOR_MAX 4

/* Allocate message vectors for a function, program its MSI-X table with them
 * and enable MSI-X. Returns the number of vectors armed (>= 1), or a negative
 * ARM_DRIVER_ERROR*. On success the caller owns the vectors and must arm each
 * `intid` with IRQ_SetHandler + IRQ_Enable. */
int32_t pci_msix_arm(const struct dwc_pcie_dev *dev, uint32_t nvec_max,
		       MSI_VECTOR *vectors);

/* Disable MSI-X on the function and withdraw its translations. */
int32_t pci_msix_disarm(const struct dwc_pcie_dev *dev);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_PCI_MSIX_H */