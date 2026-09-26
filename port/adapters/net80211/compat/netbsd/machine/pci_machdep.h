/*
 * @file   machine/pci_machdep.h
 * @brief  route to the arm32 PCI machine-dependent definitions carried
 *         in the import (sys/arch/arm/include/pci_machdep.h, reached as
 *         <arm/include/...> with sys/arch on the include path).
 *
 * Native-pcie-note: the abandoned fdt line defined __HAVE_PCI_CONF_HOOK
 * here because pcihost_fdt assigns the hook unconditionally; the native
 * glue (pcie_glue.c) fills the chipset tag directly and does not carry
 * the hook, so it stays undefined and the struct member disappears.
 */

#ifndef _COMPAT_MACHINE_PCI_MACHDEP_H_
#define _COMPAT_MACHINE_PCI_MACHDEP_H_

#include <arm/include/pci_machdep.h>

#endif /* _COMPAT_MACHINE_PCI_MACHDEP_H_ */
