/*
 * @file   rk3568_sdmmc.c
 * @brief  The two SDMMC-family controllers of this board, as their drivers'
 *         board tables. Everything here is board data - register coordinates
 *         and interrupt numbers from the verified board facts (see
 *         docs/evidence/ and DESIGN §10); the drivers themselves know none
 *         of it beyond the config structs.
 *
 *              dw-mmc (sdmmc0)            dwcmshc (eMMC)
 *   base       0xFE2B0000                 0xFE310000
 *   irq        130 (GIC_SPI 98 + 32)      51 (GIC_SPI 19 + 32)
 *   role       SDIO card slot             storage (8-bit, 52MHz cap)
 *              (RTL8189FTV WiFi since
 *              2026-09-12; no storage)
 *
 * sdmmc1 (0xFE2C0000) exists on the SoC but is not wired on this board and
 * has no table entry the adapter would use; the dw-mmc driver still carries
 * its instance coordinates for the CRU field layout.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stddef.h>

#include "dwmmc.h"
#include "dwcmshc.h"

/* --- dw-mmc (sdmmc0/sdmmc1) ------------------------------------------------- */

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

/* --- dwcmshc (eMMC) ---------------------------------------------------------- */

static const struct dwc_mshc_config dwc_mshc_configs[] = {
	{ 0U, 0xFE310000UL, 51U },	/* eMMC */
};

const struct dwc_mshc_config *dwc_mshc_config(unsigned int id)
{
	if (id >= (sizeof(dwc_mshc_configs) / sizeof(dwc_mshc_configs[0]))) {
		return NULL;
	}
	return &dwc_mshc_configs[id];
}
