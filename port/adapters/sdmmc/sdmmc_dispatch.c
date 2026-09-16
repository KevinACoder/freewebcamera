/*
 * @file   sdmmc_dispatch.c
 * @brief  The fsl_sdmmc host dispatcher: SD_CfgInitialize / MMC_CfgInitialize
 *         / SDIO_CfgInitialize and the SDMMCHOST_Init routing by hostType.
 *
 * Ported verbatim from the standalone line's host/fsl_sdmmc.c (BSD-3,
 * decision D27); the FSDIF/FSDIF_V2/FSDMMC backends are compile-time absent
 * because their CONFIG_ switches are undefined in this integration.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "fsl_sdmmc.h"
#include "fsl_sdmmc_common.h"
#include "fsl_sdmmc_host.h"

extern status_t dwmmc_host_Init(sdmmchost_t *host);
extern status_t dwmshc_host_Init(sdmmchost_t *host);
extern status_t dwmshc_host_SDConfig(sdmmc_sd_t *sdmmc, sdmmchost_config_t *config);
extern status_t dwmshc_host_MMCConfig(sdmmc_mmc_t *sdmmc, sdmmchost_config_t *config);
/*******************************************************************************
 * Code
 ******************************************************************************/
#if defined(CONFIG_FSL_SDMMC_ENABLE_SD)
status_t SD_CfgInitialize(sdmmc_sd_t *sdmmc, sdmmchost_config_t *config)
{
    assert(sdmmc);
    assert(config);
    status_t status = kStatus_Fail;
    sdmmchost_t *host = &sdmmc->host;
    sd_card_t *card = &sdmmc->card;
    sd_detect_card_t *card_cd = &sdmmc->cardDetect;

    /* link data structures */
    host->config = *config;
    host->cd = card_cd;
    card->host = host;
    host->card = card;
    card->usrParam.cd = card_cd;

    /* allocate aligned memory from innerl buffer */
    card->internalBuffer = SDMMC_OSAMemoryAlignedAllocate(host->config.maxTransSize, 
                                                          SDMMC_DATA_BUFFER_ALIGN_CACHE);
    if (NULL == card->internalBuffer)
    {
        return kStatus_OutOfRange;
    }

    memset(card->internalBuffer, 0U, host->config.maxTransSize);
    card->internalBufferSize = host->config.maxTransSize;

    SDMMC_LOG("Internal buffer@0x%x, length = 0x%x",
                card->internalBuffer,
                host->config.maxTransSize);

    host->capability = 0U;

#if defined(CONFIG_FSL_SDMMC_USE_FSDIF)
    if (kSDMMCHOST_TYPE_FSDIF == host->config.hostType)
    {
        status = FSDIFHOST_SDConfig(sdmmc, config);
    }
#endif

#if defined(CONFIG_FSL_SDMMC_USE_FSDMMC)
    if (kSDMMCHOST_TYPE_FSDMMC == host->config.hostType)
    {
        status = FSDMMCHOST_SDConfig(sdmmc, config);
    }
#endif

#if defined(CONFIG_FSL_SDMMC_USE_FSDIF_V2)
    if (kSDMMCHOST_TYPE_FSDIF_V2 == host->config.hostType)
    {
        status = FSDIFV2_HOST_SDConfig(sdmmc, config);
    }
#endif

/* no dw-mmc SD-card config: the sdmmc0 slot carries an SDIO card (see
 * sdmmc_adapter.c); the original glue's SDConfig never compiled */
#if defined(CONFIG_FSL_SDMMC_USE_FDWMSHC)
    if (kSDMMCHOST_TYPE_DWMSHC == host->config.hostType)
    {
        status = dwmshc_host_SDConfig(sdmmc, config);
    }
#endif

    if (kStatus_Success == status)
    {
        status = SD_Init(card);
    }

    return status;
}
#endif

#if defined(CONFIG_FSL_SDMMC_ENABLE_MMC)
status_t MMC_CfgInitialize(sdmmc_mmc_t *sdmmc, sdmmchost_config_t *config)
{
    assert(sdmmc);
    assert(config);
    status_t status = kStatus_Fail;
    sdmmchost_t *host = &sdmmc->host;
    mmc_card_t *card = &sdmmc->card;

    /* link data structures */
    host->config = *config;
    card->host = host;
    host->card = card;

    /* allocate aligned memory from innerl buffer */
    card->internalBuffer = SDMMC_OSAMemoryAlignedAllocate(host->config.maxTransSize, 
                                                          host->config.defBlockSize);
    if (NULL == card->internalBuffer)
    {
        return kStatus_OutOfRange;
    }

    memset(card->internalBuffer, 0U, host->config.maxTransSize);
    card->internalBufferSize = host->config.maxTransSize;

    host->capability = 0U;

#if defined(CONFIG_FSL_SDMMC_USE_FSDIF)
    if (kSDMMCHOST_TYPE_FSDIF == host->config.hostType)
    {
        status = FSDIFHOST_MMCConfig(sdmmc, config);
    }
#endif

#if defined(CONFIG_FSL_SDMMC_USE_FSDIF_V2)
    if (kSDMMCHOST_TYPE_FSDIF_V2 == host->config.hostType)
    {
        status = FSDIFV2_HOST_MMCConfig(sdmmc, config);
    }
#endif

/* no dw-mmc MMC config: unused on this board (eMMC runs on the MSHC
 * controller); the original glue's MMCConfig never compiled */
#if defined(CONFIG_FSL_SDMMC_USE_FDWMSHC)
    if (kSDMMCHOST_TYPE_DWMSHC == host->config.hostType)
    {
        status = dwmshc_host_MMCConfig(sdmmc, config);
    }
#endif

    if (kStatus_Success == status)
    {
        status = MMC_Init(card);
    }

    return status;    
}
#endif

#if defined(CONFIG_FSL_SDMMC_ENABLE_SDIO)
status_t SDIO_CfgInitialize(sdmmc_sdio_t *sdmmc, sdmmchost_config_t *config)
{
    assert(sdmmc);
    assert(config);
    status_t status = kStatus_Fail;
    sdmmchost_t *host = &sdmmc->host;
    sdio_card_t *card = &sdmmc->card;
    sd_detect_card_t *card_cd = &sdmmc->cardDetect;

    /* link data structures */
    host->config = *config;
    host->cd = card_cd;
    card->host = host;
    host->card = card;
    card->usrParam.cd = card_cd;

    /* allocate aligned memory from innerl buffer */
    card->internalBuffer = SDMMC_OSAMemoryAlignedAllocate(host->config.maxTransSize, 
                                                          host->config.defBlockSize);
    if (NULL == card->internalBuffer)
    {
        return kStatus_OutOfRange;
    }

    memset(card->internalBuffer, 0U, host->config.maxTransSize);
    card->internalBufferSize = host->config.maxTransSize;

    host->capability = 0U;

#if defined(CONFIG_FSL_SDMMC_USE_FSDIF)
    if (kSDMMCHOST_TYPE_FSDIF == host->config.hostType)
    {
        status = FSDIFHOST_SDIOConfig(sdmmc, config);
    }
#endif

    if (kStatus_Success == status)
    {
        status = SDIO_Init(card);
    }

    return status;
}
#endif

status_t SDMMCHOST_Init(sdmmchost_t *host)
{
    assert(host);
    status_t status = kStatus_Fail;

#if defined(CONFIG_FSL_SDMMC_USE_FSDIF)
    if (kSDMMCHOST_TYPE_FSDIF == host->config.hostType)
    {
        status = FSDIFHOST_Init(host);
    }
#endif

#if defined(CONFIG_FSL_SDMMC_USE_FSDMMC)
    if (kSDMMCHOST_TYPE_FSDMMC == host->config.hostType)
    {
        status = FSDMMCHOST_Init(host);
    }
#endif

#if defined(CONFIG_FSL_SDMMC_USE_FSDIF_V2)
    if (kSDMMCHOST_TYPE_FSDIF_V2 == host->config.hostType)
    {
        status = FSDIFV2_HOST_Init(host);
    }
#endif

#if defined(CONFIG_FSL_SDMMC_USE_FDWMMC)
    if (kSDMMCHOST_TYPE_DWMMC == host->config.hostType)
    {
        status = dwmmc_host_Init(host);
    }
#endif

#if defined(CONFIG_FSL_SDMMC_USE_FDWMSHC)
    if (kSDMMCHOST_TYPE_DWMSHC == host->config.hostType)
    {
        status = dwmshc_host_Init(host);
    }
#endif

    return status;
}

