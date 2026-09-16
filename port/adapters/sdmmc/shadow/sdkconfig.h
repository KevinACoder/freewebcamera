/*
 * @file   sdkconfig.h
 * @brief  Shadow of the SDKconfig header the standalone fsl_sdmmc stack is
 *         preprocessed against. Defines this integration's feature selection
 *         - the equivalents of the rk3568 sd/emmc example configs, minus the
 *         read-only switch (the eMMC is this milestone's storage target and
 *         is authorized for writes, DESIGN D28) and minus the I2C vqmmc
 *         helper (this board's rails are fixed).
 *
 * Not an interface: nothing outside the sdmmc adapter includes this.
 */

#ifndef SDMMC_SHADOW_SDKCONFIG_H
#define SDMMC_SHADOW_SDKCONFIG_H

/* The full card-protocol set compiles in; hostType picks the controller. */
#define CONFIG_FSL_SDMMC_ENABLE_SD	1
#define CONFIG_FSL_SDMMC_ENABLE_MMC	1
#define CONFIG_FSL_SDMMC_ENABLE_SDIO	1

/* The two host backends this board carries. */
#define CONFIG_FSL_SDMMC_USE_FDWMMC	1
#define CONFIG_FSL_SDMMC_USE_FDWMSHC	1

/* SDMMC_VERBOSE (logging depth) is chosen per-file, not here. */

#endif /* SDMMC_SHADOW_SDKCONFIG_H */
