/*
 * @file   rk3568_sdmmc.c
 * @brief  The SDMMC controller this line carries, as its driver's board
 *         table. Everything here is board data - register coordinates and
 *         interrupt numbers from the verified board facts (DESIGN §10); the
 *         driver itself knows none of it beyond the config structs.
 *
 *   dw-mmc (sdmmc0)
 *   base       0xFE2B0000
 *   irq        130 (GIC_SPI 98 + 32)
 *   role       SDIO card slot (RTL8189FTV WiFi since 2026-09-12; no storage)
 *
 * The eMMC controller (dwcmshc, 0xFE310000/IRQ51) and sdmmc1 (0xFE2C0000,
 * not wired on this board) stay with their own feat lines; the dw-mmc
 * driver still carries the sdmmc1 coordinates for the CRU field layout.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stddef.h>

#include "dwmmc.h"

static const struct dwc_mmc_config dwc_mmc_configs[] = {
	{ 0U, 0xFE2B0000UL, 130U, true }, /* sdmmc0: SDIO module, no CD line */
	{ 1U, 0xFE2C0000UL, 131U, false }, /* sdmmc1: not wired on this board */
};

const struct dwc_mmc_config *dwc_mmc_config(unsigned int id)
{
	if (id >= (sizeof(dwc_mmc_configs) / sizeof(dwc_mmc_configs[0]))) {
		return NULL;
	}
	return &dwc_mmc_configs[id];
}
