/*
 * @file   rk3568_sata.c
 * @brief  The two on-chip SATA controllers of this board, as coordinates for
 *         the DesignWare AHCI core.
 *
 * Board data only. The RK3568 integrates two DesignWare AHCI 1.3 controllers,
 * each with a single implemented port behind its own combo PHY:
 *
 *   controller 0   HBA 0xFC000000   the upright SATA socket
 *   controller 1   HBA 0xFC400000   the M.2 2280 slot
 *   stride         0x400000         (one controller's register window)
 *   version        1.30, 32 slots, PORTS_IMPL 0x1
 *
 * Both disks were measured at 250069680 sectors of 512 bytes (119 GB) with the
 * same driver in the embox line, which is also what U-Boot's `scsi scan`
 * reports - the geometry line this driver prints should match it exactly.
 *
 * The MMU maps 0xFC000000..0xFC800000 Device already (it is inside the top
 * 1GiB window), so this file carries no mapping requirement.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>

#include "dwc_ahci.h"

const struct dwc_ahci_plat dwc_ahci_plat = {
	.base_addr = 0xFC000000UL,
	.ctrl_count = 2,
	.port_count = 1,	/* one implemented port per controller */
	.stride = 0x400000UL,
};