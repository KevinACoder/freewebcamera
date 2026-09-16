/*
 * @file   sdmmc_adapter.c
 * @brief  The SDIO card on the sdmmc0 slot (an RTL8189FTV WiFi module):
 *         bus enumeration through the standalone fsl_sdmmc SDIO flow, CIS
 *         printout and a CMD52 read-back check.
 *
 * Scope for this milestone is enumeration only: get the card through CMD5 ->
 * CMD3 -> CMD7, read its CCCR/FBR/CIS over CMD52, switch to high speed at
 * 3.3V, and say who is out there. The wireless function itself is a later
 * milestone; nothing here touches CMD53 data or the WLAN function registers
 * beyond what enumeration reads.
 *
 * The structure linking and buffer staging follow the dispatcher's
 * SDIO_CfgInitialize body; the per-host SDIOConfig step is absent there for
 * this backend, so the capability fields are filled here exactly as the
 * example configs do.
 *
 * Board facts this adapter bakes in (see DESIGN §10): the module is
 * soldered to the slot, always powered, no 1.8V switch (no CMD11), card
 * detect is host-CD and always affirmative.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "board.h"
#include "fsl_sdmmc.h"
#include "fsl_sdmmc_host.h"

/* The bus clock ceiling for the SDIO card: high speed at 3.3V. */
#define SDIO_BUS_CLOCK_HZ	50000000U
#define SDIO_MAX_TRANS_BYTES	(64U * 1024U)
#define SDIO_DEF_BLOCK_SIZE	512U

static sdmmc_sdio_t sdio_sdmmc;
static bool sdio_ready;

bool sdio_card_up(void)
{
	return sdio_ready;
}

static int sdio_init_once(void)
{
	sdmmchost_config_t config;
	sdio_card_t *card;
	sd_detect_card_t *card_cd;
	sdmmchost_t *host;
	status_t status;

	memset(&sdio_sdmmc, 0, sizeof(sdio_sdmmc));
	memset(&config, 0, sizeof(config));

	config.hostId = 0U;
	config.hostType = kSDMMCHOST_TYPE_DWMMC;
	config.cardType = kSDMMCHOST_CARD_TYPE_SDIO;
	config.enableIrq = false;
	config.enableDMA = true;
	config.endianMode = kSDMMCHOST_EndianModeLittle;
	config.maxTransSize = SDIO_MAX_TRANS_BYTES;
	config.defBlockSize = SDIO_DEF_BLOCK_SIZE;
	config.cardClock = SDIO_BUS_CLOCK_HZ;
	config.isUHSCard = false; /* no 1.8V switch: stays 3.3V */

	host = &sdio_sdmmc.host;
	card = &sdio_sdmmc.card;
	card_cd = &sdio_sdmmc.cardDetect;

	/* link data structures (the SDIO_CfgInitialize prologue) */
	host->config = config;
	host->cd = card_cd;
	card->host = host;
	host->card = card;
	card->usrParam.cd = card_cd;

	card_cd->type = kSD_DetectCardByHostCD;
	card_cd->cdDebounce_ms = 10U;

	/* stage the card's internal buffer */
	card->internalBuffer = SDMMC_OSAMemoryAlignedAllocate(
		config.maxTransSize, config.defBlockSize);
	if (card->internalBuffer == NULL) {
		return -1;
	}
	memset(card->internalBuffer, 0, config.maxTransSize);
	card->internalBufferSize = config.maxTransSize;

	host->capability = 0U;

	/* card-side expectations: 4-bit, high speed, fixed 3.3V rails */
	card->usrParam.ioVoltage = NULL; /* never switches to 1.8V */
	card->usrParam.maxFreq = SDIO_BUS_CLOCK_HZ;
	card->usrParam.capability = (uint32_t)kSDMMCHOST_Support4BitDataWidth |
				    (uint32_t)kSDMMCHOST_SupportHighSpeed |
				    (uint32_t)kSDMMCHOST_SupportVoltage3v3;

	host->capability |= (uint32_t)kSDMMCHOST_Support4BitDataWidth |
			    (uint32_t)kSDMMCHOST_SupportHighSpeed |
			    (uint32_t)kSDMMCHOST_SupportVoltage3v3 |
			    (uint32_t)kSDMMCHOST_SupportAutoCmd12;
	host->maxBlockCount = (uint32_t)(config.maxTransSize / config.defBlockSize);
	host->maxBlockSize = SDMMCHOST_SUPPORT_MAX_BLOCK_LENGTH;
	host->sourceClock_Hz = 150000000U; /* dw-mmc ciu source */

	if (kStatus_Success != SDIO_Init(card)) {
		board_log("sdio: card init failed (no card or bus error)\n");
		return -1;
	}

	{
		/* CIS manufacturer code + information: for the RTL8189FTV
		 * this is 024c:f179, the pair the linux line reports as
		 * vendor/device */
		uint8_t cccr_rev = 0U;

		/* CMD52 read-back: the enumeration has hammered CMD52
		 * already; this one is the explicit loopback proof */
		if (kStatus_Success !=
		    SDIO_IO_Read_Direct(card, kSDIO_FunctionNum0,
					kSDIO_RegCCCRSdioVer, &cccr_rev)) {
			board_log("sdio: CCCR read-back failed\n");
			return -1;
		}

		board_log("sdio: card %03x:%04x, %u IO func(s), CCCR v%u.%u, bus %u kHz%s\n",
			  card->commonCIS.mID,
			  card->commonCIS.mInfo,
			  card->ioTotalNumber,
			  (card->cccrVersioin >> 4U) & 0x7U,
			  card->cccrVersioin & 0xFU,
			  card->busClock_Hz / 1000U,
			  (card->currentTiming == kSD_TimingSDR25HighSpeedMode)
				  ? " (HS)"
				  : "");
	}

	return 0;
}

int sdio_start(void)
{
	if (sdio_ready) {
		return 0;
	}

	if (sdio_init_once() != 0) {
		return -1;
	}

	sdio_ready = true;
	return 0;
}

int sdio_restart(void)
{
	if (sdio_ready) {
		SDIO_Deinit(&sdio_sdmmc.card);
		sdio_ready = false;
	}
	return sdio_start();
}

sdio_card_t *sdio_card_get(void)
{
	return sdio_ready ? &sdio_sdmmc.card : NULL;
}
