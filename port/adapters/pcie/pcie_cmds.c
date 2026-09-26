/*
 * @file
 * @brief pcie shell command: bring the DesignWare root complexes up and
 * dump what the firmware left on them.
 *
 * Everything runs through the frozen ARM_DRIVER_PCIE interface
 * (include/pcie.h) - the command never sees the driver's internal
 * headers, which is also what makes it a conformance check for that
 * interface: if a dump needs something the interface cannot express,
 * the interface (not the command) grows it.
 *
 * @author zhugengyu
 * @date 26.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "pcie.h"

#include "cherrysh_adapter.h"
#include "csh.h"

extern ARM_DRIVER_PCIE Driver_PCIe;

/* the pcie glue's interrupt forensics (pcie_glue.c) */
extern unsigned long pcie_glue_irq_counts(unsigned long *spurious);

/* client APB legacy-interrupt block (pcie3x2, where the AC7260 sits) */
#define PCIE3X2_APB_BASE	0xfe280000UL
#define PCIE_CLIENT_INT_STATUS	0x08
#define PCIE_CLIENT_INT_MASK	0x1c

/* the two controller instances the board table carries; their
 * downstream bus numbers are board facts (port/board/rk3568/
 * rk3568_pcie.c: pcie2x1 root bus 0, pcie3x2 root bus 2) */
#define PCIE_INSTANCES	2
static const uint32_t pcie_downstream_bus[PCIE_INSTANCES] = { 1, 3 };

#define PCIE_CFG_VENDOR_ID	0x00
#define PCIE_CFG_COMMAND	0x04
#define PCIE_CFG_CLASS		0x08
#define PCIE_CFG_CAP_PTR	0x34
#define PCIE_CFG_BAR0		0x10

#define PCIE_CAP_ID_MSI		0x05
#define PCIE_CAP_ID_MSIX	0x11

/* config read helper: one aligned dword via the interface */
static int pcie_cfg_rd(chry_shell_t *csh, uint32_t bus, uint32_t devfn,
	uint32_t off, uint32_t *val)
{
	PCIE_CONFIG_ACCESS acc;

	acc.bus = bus;
	acc.device = (devfn >> 3) & 0x1fu;
	acc.function = devfn & 7u;
	acc.offset = off;
	acc.value = val;

	*val = 0xffffffffu;
	if (Driver_PCIe.ConfigRead(&acc) != ARM_DRIVER_OK && csh != NULL) {
		csh_printf(csh, "  bus %u devfn %u reg %02x: read failed\r\n",
		    bus, devfn, off);
		return -1;
	}
	return 0;
}

static void pcie_dump_function(chry_shell_t *csh, uint32_t bus,
	uint32_t devfn)
{
	uint32_t id, class, cmd, bar[6];
	uint32_t next, guard;
	int printed;

	if (pcie_cfg_rd(csh, bus, devfn, PCIE_CFG_VENDOR_ID, &id) != 0) {
		return;
	}
	if (id == 0xffffffffu || (id & 0xffffu) == 0xffffu) {
		return;	/* nothing answers here (or never will) */
	}

	(void) pcie_cfg_rd(csh, bus, devfn, PCIE_CFG_CLASS, &class);
	(void) pcie_cfg_rd(csh, bus, devfn, PCIE_CFG_COMMAND, &cmd);

	csh_printf(csh, "  %02x:%02x.%u %04x:%04x class %02x%02x cmd %04x\r\n",
	    bus, devfn >> 3, devfn & 7u, id & 0xffffu, id >> 16,
	    (class >> 24) & 0xffu, (class >> 16) & 0xffu, cmd & 0xffffu);

	printed = 0;
	for (uint32_t i = 0; i < 6u; i++) {
		if (pcie_cfg_rd(csh, bus, devfn,
		    PCIE_CFG_BAR0 + 4u * i, &bar[i]) == 0 && bar[i] != 0) {
			csh_printf(csh, "    BAR%u = %08x%s\r\n", i, bar[i],
			    (bar[i] & 0x1u) ? " (io)" :
			    ((bar[i] & 0x6u) == 0x4u) ? " (mem64)" : " (mem)");
			printed = 1;
		}
		if ((bar[i] & 0x6u) == 0x4u && i < 5u) {
			i++;	/* the upper half occupies the next slot */
		}
	}
	if (!printed) {
		csh_printf(csh, "    (BARs unassigned - firmware sizing step"
		    " missing?)\r\n");
	}

	/* capability walk (mirrors the driver's own scan) */
	next = 0;
	(void) pcie_cfg_rd(csh, bus, devfn, PCIE_CFG_CAP_PTR, &next);
	csh_printf(csh, "    cap ptr = %02x\r\n", next & 0xfcu);
	next &= 0xfcu;
	for (guard = 0; guard < 48u && next != 0; guard++) {
		uint32_t hdr, id_lo;

		if (next < 0x40u || next >= 0x100u) {
			csh_printf(csh, "    cap walk: out-of-range next=%02x,"
			    " stopping\r\n", next);
			break;
		}
		if (pcie_cfg_rd(csh, bus, devfn, next, &hdr) != 0) {
			break;
		}
		id_lo = hdr & 0xffu;
		if (id_lo == PCIE_CAP_ID_MSI) {
			csh_printf(csh, "    cap MSI   @%02x (ctrl %04x)\r\n",
			    next, (hdr >> 16) & 0xffffu);
		} else if (id_lo == PCIE_CAP_ID_MSIX) {
			uint32_t tbl;

			(void) pcie_cfg_rd(csh, bus, devfn, next + 4u, &tbl);
			csh_printf(csh, "    cap MSI-X @%02x (ctrl %04x table"
			    " BAR%u+%08x)\r\n", next, hdr >> 16,
			    tbl & 7u, tbl & ~7u);
		} else {
			csh_printf(csh, "    cap id %02x @%02x\r\n", id_lo,
			    next);
		}
		/* the next pointer is bits 15:8 of the same dword (reading
		 * next+1 would align back down and re-read the id byte,
		 * which stopped the walk after the first capability) */
		next = (hdr >> 8) & 0xfcu;
	}
}

static int cmd_pcie(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	if (argc >= 2 && strcmp(argv[1], "init") == 0) {
		for (uint32_t i = 0; i < PCIE_INSTANCES; i++) {
			int32_t ret = Driver_PCIe.Initialize(i, NULL);

			csh_printf(csh, "pcie%u: initialize %s\r\n", i,
			    ret == ARM_DRIVER_OK ? "ok (link inherited)" :
			    "no link (run U-Boot 'pci enum' first)");
		}
		return 0;
	}

	if (argc < 2 || strcmp(argv[1], "dump") != 0) {
		csh_printf(csh, "usage: pcie init | dump\r\n");
		return 0;
	}

	for (uint32_t i = 0; i < PCIE_INSTANCES; i++) {
		uint32_t ltssm = 0;
		uint32_t count = 0;
		uint32_t cpu_addr = 0, size = 0;
		PCIE_CAPABILITIES caps;

		if (Driver_PCIe.Initialize(i, NULL) != ARM_DRIVER_OK) {
			csh_printf(csh, "pcie%u: no link inherited"
			    " (LTSSM says firmware never trained it)\r\n", i);
			continue;
		}
		(void) Driver_PCIe.GetLinkState(i, &ltssm);
		caps = Driver_PCIe.GetCapabilities(i);
		(void) Driver_PCIe.Enumerate(i, &count);
		(void) Driver_PCIe.MapWindow(i, 0, &cpu_addr, &size);

		csh_printf(csh, "pcie%u: ltssm=%08x gen%u x%u,"
		    " devices %u, mem window %08x+%x\r\n",
		    i, ltssm, caps.link_speed, caps.max_lanes, count,
		    cpu_addr, size);

		/* pcie3x2 (instance 1): legacy-interrupt block forensics */
		if (i == 1u) {
			volatile uint32_t *apb =
			    (volatile uint32_t *) PCIE3X2_APB_BASE;
			unsigned long spurious = 0;
			unsigned long nirq = pcie_glue_irq_counts(&spurious);
			uint32_t bar0 = 0;

			csh_printf(csh, "    intx status=%08x mask=%08x,"
			    " glue irq=%lu spurious=%lu\r\n",
			    apb[PCIE_CLIENT_INT_STATUS / 4u],
			    apb[PCIE_CLIENT_INT_MASK / 4u], nirq, spurious);

			/* Radio BAR0 through the outbound MEM window: the
			 * config path works, but the window itself was never
			 * exercised.  CSR_INT (0x008) is deliberately NOT
			 * read: it is read-clear, and a dump taken while the
			 * radio is live would eat a pending interrupt cause.
			 * INT_MASK/GP_CNTRL/HW_REV are plain reads. */
			if (pcie_cfg_rd(csh, pcie_downstream_bus[i], 0,
			    PCIE_CFG_BAR0, &bar0) == 0) {
				volatile uint32_t *b =
				    (volatile uint32_t *)(uintptr_t)
				    (bar0 & ~0xfu);

				csh_printf(csh, "    radio bar0 %08x mmio:"
				    " 000=%08x 00c=%08x"
				    " 024=%08x 028=%08x\r\n",
				    (uint32_t)(uintptr_t) b,
				    b[0x000 / 4], b[0x00c / 4],
				    b[0x024 / 4], b[0x028 / 4]);
			}
			/* NVMe CAP (read-only) as the known-good control
			 * for the same MEM window */
			{
				volatile uint32_t *n =
				    (volatile uint32_t *)(uintptr_t)
				    0xf4300000UL;

				csh_printf(csh, "    nvme bar0 mmio:"
				    " 000=%08x 008=%08x\r\n", n[0], n[2]);
			}
		}

		/* the controller's downstream bus: slot 0 only, the flat
		 * scan the driver ran is what this mirrors */
		pcie_dump_function(csh, pcie_downstream_bus[i], 0);
		for (uint32_t f = 1; f < 8u; f++) {
			/* multi-function endpoints: each function costs one
			 * config read on the vendor id */
			pcie_dump_function(csh, pcie_downstream_bus[i],
					   f);
		}
	}

	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_pcie, pcie, "pcie init | dump",
			  "DesignWare PCIe host: firmware-inherited link and devices");
