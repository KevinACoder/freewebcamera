/*
 * @file   opt_pci.h
 * @brief  stand-in for the config(8)-generated PCI flag header.
 *
 * PCI_NETBSD_CONFIGURE stays off: the embedded world keeps the firmware
 * BAR assignment (no resource allocator runs - the driver never writes
 * a BAR; sizing one would have to overwrite the assignment).
 *
 * __HAVE_PCI_MSI_MSIX is on: pcie_glue.c implements the pci_intr_alloc
 * family. Classic MSI rides the board-proven doorbell path (endpoint
 * message write -> inbound iATU -> GITS_TRANSLATER -> ITS -> LPI, the
 * path the NVMe line validated); INTx is the fallback - the client-APB
 * legacy aggregator was armed but never exercised end to end.
 */

#ifndef _COMPAT_OPT_PCI_H_
#define _COMPAT_OPT_PCI_H_

/* #define PCI_NETBSD_CONFIGURE 1 */
#define __HAVE_PCI_MSI_MSIX 1

#endif /* _COMPAT_OPT_PCI_H_ */
