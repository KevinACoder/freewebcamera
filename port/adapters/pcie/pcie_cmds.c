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
	next &= 0xfcu;
	for (guard = 0; guard < 48u && next != 0; guard++) {
		uint32_t hdr, id_lo;

		if (next < 0x40u || next >= 0x100u) {
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
		}
		(void) pcie_cfg_rd(csh, bus, devfn, next + 1u, &next);
		next &= 0xfcu;
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
