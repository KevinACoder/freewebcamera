/*
 * @file   sdmmc_cmds.c
 * @brief  Shell commands for the SDMMC family: the operational view the
 *         acceptance runs are written against.
 *
 *   sdio                    the SDIO card on the sdmmc0 slot at a glance
 *   sdio reinit             re-run the SDIO enumeration from scratch
 *   sdio cis                the card's CIS tuples and FBR block sizes
 *
 * The eMMC/storage view of the old adapter rides with the storage feat line,
 * not here.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "fsl_sdio.h"
#include "fsl_sdmmc_common.h"

#include "cherrysh_adapter.h"
#include "csh.h"

/* Provided by sdmmc_adapter.c in this adapter. */
extern bool sdio_card_up(void);
extern sdio_card_t *sdio_card_get(void);
extern int sdio_restart(void);

/* --- the SDIO view ----------------------------------------------------------- */

static void sdio_print(chry_shell_t *csh)
{
	const sdio_card_t *card = sdio_card_get();

	if (card == NULL) {
		csh_printf(csh, "sdio0 (not initialized or init failed)\n");
		return;
	}

	csh_printf(csh, "sdio0 %03x:%04x, %u IO func(s), sdio v%u, cccr v%u.%u\n",
		   card->commonCIS.mID, card->commonCIS.mInfo,
		   card->ioTotalNumber, card->sdioVersion,
		   (card->cccrVersioin >> 4U) & 0x7U,
		   card->cccrVersioin & 0xFU);
	csh_printf(csh, "      bus %u kHz, timing %d, rca %u, ocr 0x%06x\n",
		   card->busClock_Hz / 1000U, (int)card->currentTiming,
		   card->relativeAddress, card->ocr & 0xFFFFFFU);
	csh_printf(csh, "      cis: fnid %u, fn0 maxblk %u, max speed 0x%02x\n",
		   card->commonCIS.funcID, card->commonCIS.fn0MaxBlkSize,
		   card->commonCIS.maxTransSpeed);
}

static int cmd_sdio(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	const sdio_card_t *card;

	if ((argc >= 2) && (strcmp(argv[1], "reinit") == 0)) {
		csh_printf(csh, "sdio: re-enumerating\r\n");
		if (sdio_restart() != 0) {
			csh_printf(csh, "sdio: init failed\r\n");
		}
		sdio_print(csh);
		return 0;
	}

	if ((argc >= 2) && (strcmp(argv[1], "cis") == 0)) {
		card = sdio_card_get();
		if (card == NULL) {
			csh_printf(csh, "sdio: no card\r\n");
			return -1;
		}
		csh_printf(csh, "common cis: mID %04x mInfo %04x fnid %u fn0blk %u speed %02x\r\n",
			   card->commonCIS.mID, card->commonCIS.mInfo,
			   card->commonCIS.funcID,
			   card->commonCIS.fn0MaxBlkSize,
			   card->commonCIS.maxTransSpeed);
		for (uint32_t i = 0U; i < FSL_SDIO_MAX_IO_NUMS; i++) {
			if (card->ioFBR[i].ioStdFunctionCode == 0U) {
				continue;
			}
			csh_printf(csh, "fbr[%u]: std fn %u, ext %02x, cis %08x, blk %u\r\n",
				   i, card->ioFBR[i].ioStdFunctionCode,
				   card->ioFBR[i].ioExtFunctionCode,
				   card->ioFBR[i].ioPointerToCIS,
				   card->ioFBR[i].ioBlockSize);
		}
		return 0;
	}

	if (!sdio_card_up()) {
		csh_printf(csh, "usage: sdio [reinit|cis]\r\n");
		return -1;
	}

	sdio_print(csh);
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_sdio, sdio,
			  "sdio [reinit|cis]",
			  "SDIO card on the sdmmc0 slot");
