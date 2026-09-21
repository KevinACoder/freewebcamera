/*
 * @file   rk3568_pcie.c
 * @brief  The two PCIe controllers of this board, as coordinates for the
 *         DesignWare root complex backend.
 *
 * Everything here is board data. The core (dwc_pcie.c) knows no addresses -
 * this file is the RK3568 half of that split and lives in port/board/rk3568/
 * with the rest of the platform glue (D45): drivers/ keeps only
 * platform-agnostic IP code, board data and SoC integration live here.
 *
 *              pcie2x1 (the M.2 slot)   pcie3x2 (the x4 slot)
 *   APB        0xFE260000               0xFE280000
 *   DBI        0x3C0000000              0x3C0800000            (above 4GiB)
 *   config     0xF4000000               0xF0000000
 *   MEM window 0xF4200000 + 30MiB       0xF0200000 + 30MiB
 *   buses      0 (root port), 1         2 (root port), 3
 *   INTx       104                      194
 *
 * The INTx numbers are informational only: neither controller's legacy
 * interrupt is routed anywhere on this board, and both NVMe and (later) the
 * WiFi endpoints use MSI/MSI-X through the ITS instead.
 *
 * Two numbers here have a second home that must stay in agreement:
 *   - the DBI frames are mapped Device in port/board/common/mmu.c (they are above
 *     4GiB, i.e. outside the identity map's low 4GiB coverage), as L1[15];
 *   - the doorbell window the core programs into the inbound iATU is
 *     BOARD_ITS_TRANSLATER from port/board/board.h, not a value from here:
 *     the GIC is one device in the SoC, shared by both controllers.
 *
 * The MEM windows are where the endpoints' BARs were assigned by firmware
 * (0xF4300000 for the NVMe, as measured) - the core maps the window identity
 * and relies on that assignment rather than moving anything.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>

#include "dwc_pcie.h"

const struct dwc_pcie_plat dwc_pcie_plats[DWC_PCIE_CTRL_COUNT] = {
	{
		.apb_base = 0xFE260000UL,
		.dbi_base = 0x3C0000000ULL,
		.cfg_base = 0xF4000000ULL,
		.mem_base = 0xF4200000ULL,
		.mem_size = 0x1E00000ULL,	/* 30 MiB */
		.bus_base = 0,
		.intx_irq = 104,
	},
	{
		.apb_base = 0xFE280000UL,
		.dbi_base = 0x3C0800000ULL,
		.cfg_base = 0xF0000000ULL,
		.mem_base = 0xF0200000ULL,
		.mem_size = 0x1E00000ULL,
		.bus_base = 2,
		.intx_irq = 194,
	},
};