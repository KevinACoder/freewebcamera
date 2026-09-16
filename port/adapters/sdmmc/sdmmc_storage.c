/*
 * @file   sdmmc_storage.c
 * @brief  The eMMC as a block device: the standalone fsl_sdmmc MMC flow
 *         (MMC_CfgInitialize over the DWC MSHC glue) adapted to
 *         include/blkdev.h ops.
 *
 * The init sequence is the standalone example's, unmodified: fill the host
 * config (8-bit, 52MHz ceiling - the board has no 52M tap and HS200 does
 * not pass on this wiring), MMC_CfgInitialize links the host/card pair,
 * stages the card's internal buffer and runs the glue's MMCConfig + the
 * protocol's MMC_Init. One guard was added: a card that enumerates with a
 * zero size is re-initialized once, because that is the state the
 * firmware's own failed probe leaves behind (KI-025: the second full init
 * is what recovers it, measured in u-boot).
 *
 * Buffers arrive from the FatFs layer with arbitrary alignment; the
 * protocol's word-pointer data contract needs 4-byte-aligned, word
 * -accessible buffers, so every request is staged through a static aligned
 * bounce area (64 KiB, the largest single request the block layer makes).
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "Driver_Common.h"
#include "blkdev.h"
#include "sdkconfig.h"

#include "board.h"
#include "fsl_sdmmc.h"
#include "fsl_sdmmc_host.h"
#include "sdmmc_board.h"

/* The card clock ceiling, and the one measured-good operating point:
 * 24MHz from the 24M CRU source, 8-bit bus. The higher taps are unusable for
 * host->card data on this board (measured 2026-09-16, cold boots each time):
 *   - 50M source, bypass (50MHz) or /2 (25MHz): reads fine, but CMD24 writes
 *     answer DATA_CRC | DATA_END_BIT - the card sees corrupt DAT bytes;
 *   - 100M source, /2 (52MHz): enumeration itself fails (CMD6 CMD_INDEX
 *     errors, card falls back to 4-bit, CMD17/CMD12 time out);
 *   - 24M source, bypass (24MHz): reads AND writes clean, card enumerated
 *     8-bit/HS, 29824 MiB.
 * The reference driver only ever proved reads at 50MHz, so its working point
 * does not cover the write direction the milestone needs. */
#define EMMC_BUS_CLOCK_HZ	24000000U
/* 64 blocks: the size the standalone reference measured good on this board
 * (it also keeps host->maxBlockCount at 64 and each SDMA run well inside one
 * 512 KiB boundary). */
#define EMMC_MAX_TRANS_BYTES	(64U * EMMC_BLOCK_SIZE)
#define EMMC_BLOCK_SIZE		512U

static sdmmc_mmc_t emmc_sdmmc;
static bool emmc_ready;

/* Bounce area for misaligned caller buffers (FatFs hands us whatever the
 * application allocated). 64 KiB covers the block layer's largest request. */
static uint32_t emmc_bounce[EMMC_MAX_TRANS_BYTES / sizeof(uint32_t)]
	__attribute__((aligned(64)));

/* --- init ---------------------------------------------------------------- */

bool sdmmc_storage_ready(void)
{
	return emmc_ready;
}

static int emmc_init_once(void)
{
	sdmmchost_config_t config;
	status_t status;

	memset(&emmc_sdmmc, 0, sizeof(emmc_sdmmc));
	memset(&config, 0, sizeof(config));

	config.hostId = 0U;
	config.hostType = kSDMMCHOST_TYPE_DWMSHC;
	config.cardType = kSDMMCHOST_CARD_TYPE_EMMC;
	config.enableIrq = false;
	config.enableDMA = true;
	config.endianMode = kSDMMCHOST_EndianModeLittle;
	config.maxTransSize = EMMC_MAX_TRANS_BYTES;
	config.defBlockSize = EMMC_BLOCK_SIZE;
	config.cardClock = EMMC_BUS_CLOCK_HZ;
	config.isUHSCard = false;

	/* the standalone flow: link structures, stage the card's internal
	 * buffer, run the glue's MMCConfig, then MMC_Init */
	status = MMC_CfgInitialize(&emmc_sdmmc, &config);
	if (kStatus_Success != status) {
		board_log("sdmmc: eMMC init failed (status %d)\n", status);
		return -1;
	}

	board_log("sdmmc: CSD struct=%u spec=%u, cardType=0x%02x, flags=0x%x\n",
		  emmc_sdmmc.card.csd.csdStructureVersion,
		  emmc_sdmmc.card.csd.systemSpecificationVersion,
		  emmc_sdmmc.card.extendedCsd.cardType,
		  emmc_sdmmc.card.flags);
	{
		/* EXT_CSD raw content probe: is the card's data actually
		 * landing in the internal buffer? bytes 192 (REV), 196
		 * (CARD_TYPE) must be non-zero on any eMMC 4.x+ part. */
		const uint8_t *raw = emmc_sdmmc.card.internalBuffer;

		board_log("sdmmc: ext_csd[192]=%02x [196]=%02x [3]=%02x [160]=%02x\n",
			  raw[192], raw[196], raw[3], raw[160]);
	}

	/* Geometry guard: the size comes from EXT_CSD. A zero block count
	 * with a live CID is the state the firmware's own failed probe
	 * leaves behind - the second full init recovers it (KI-025). */
	if (emmc_sdmmc.card.userPartitionBlocks == 0U) {
		board_log("sdmmc: eMMC enumerated but size reads 0 - re-init\n");
		return -1;
	}

	{
		/* CID product name is not NUL-terminated; copy before printing */
		char name[sizeof(emmc_sdmmc.card.cid.productName) + 1U];

		memcpy(name, emmc_sdmmc.card.cid.productName,
		       sizeof(emmc_sdmmc.card.cid.productName));
		name[sizeof(emmc_sdmmc.card.cid.productName)] = '\0';
		board_log("sdmmc: eMMC man 0x%02x %s, %u MiB, %u-bit %uMHz\n",
			  emmc_sdmmc.card.cid.manufacturerID,
			  name,
			  (unsigned int)(((uint64_t)emmc_sdmmc.card.userPartitionBlocks *
					  emmc_sdmmc.card.blockSize) /
					 (1024U * 1024U)),
			  (emmc_sdmmc.card.busWidth == kMMC_DataBusWidth8bit) ? 8U : 4U,
			  (unsigned int)(emmc_sdmmc.card.busClock_Hz / 1000000U));
	}

	return 0;
}

int sdmmc_storage_init(void)
{
	if (emmc_ready) {
		return 0;
	}

	if (emmc_init_once() != 0) {
		/* second complete pass, including a fresh internal buffer
		 * (aligned allocations carry their raw pointer in front, so
		 * they must go through the aligned free) */
		if (emmc_sdmmc.card.internalBuffer != NULL) {
			SDMMC_OSAMemoryAlignedFree(emmc_sdmmc.card.internalBuffer);
		}
		memset(&emmc_sdmmc, 0, sizeof(emmc_sdmmc));
		if (emmc_init_once() != 0) {
			return -1;
		}
	}

	emmc_ready = true;
	return 0;
}

int sdmmc_storage_restart(void)
{
	if (emmc_ready) {
		/* the blkdev registration stays; ops fail until init passes */
		emmc_ready = false;
	}
	return sdmmc_storage_init();
}

bool sdmmc_storage_caps(BLKDEV_CAPABILITIES *caps)
{
	if (!emmc_ready) {
		return false;
	}

	caps->sector_count = emmc_sdmmc.card.userPartitionBlocks;
	caps->sector_size = emmc_sdmmc.card.blockSize;
	caps->max_transfer = EMMC_MAX_TRANS_BYTES;
	caps->erase_block_size =
		emmc_sdmmc.card.eraseGroupBlocks * emmc_sdmmc.card.blockSize;
	caps->writable = 1;
	caps->media_present = 1;

	return true;
}

/* --- the blkdev wrapper --------------------------------------------------- */

static int32_t emmc_read(uint32_t unit, uint64_t lba, void *data, uint32_t bytes)
{
	uint32_t sector_size = emmc_sdmmc.card.blockSize;
	uint32_t sectors;
	uint8_t *dst = (uint8_t *)data;
	uint64_t block = lba;
	uint32_t done = 0U;

	if ((0U != unit) || !emmc_ready) {
		return ARM_DRIVER_ERROR;
	}
	if ((sector_size == 0U) || ((bytes % sector_size) != 0)) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	sectors = bytes / sector_size;
	while (done < bytes) {
		uint32_t chunk_sectors = sectors;
		uint32_t chunk_bytes;

		if (chunk_sectors > (EMMC_MAX_TRANS_BYTES / sector_size)) {
			chunk_sectors = EMMC_MAX_TRANS_BYTES / sector_size;
		}
		chunk_bytes = chunk_sectors * sector_size;

		/* stage through the bounce area: the protocol wants word
		 * pointers and the controller wants alignment */
		if (kStatus_Success != MMC_ReadBlocks(&emmc_sdmmc.card,
						      (uint8_t *)emmc_bounce,
						      (uint32_t)block,
						      chunk_sectors)) {
			return ARM_DRIVER_ERROR;
		}
		memcpy(dst + done, emmc_bounce, chunk_bytes);

		block += chunk_sectors;
		done += chunk_bytes;
		sectors -= chunk_sectors;
	}

	return ARM_DRIVER_OK;
}

static int32_t emmc_write(uint32_t unit, uint64_t lba, const void *data,
			  uint32_t bytes)
{
	uint32_t sector_size = emmc_sdmmc.card.blockSize;
	uint32_t sectors;
	const uint8_t *src = (const uint8_t *)data;
	uint64_t block = lba;
	uint32_t done = 0U;

	if ((0U != unit) || !emmc_ready) {
		return ARM_DRIVER_ERROR;
	}
	if ((sector_size == 0U) || ((bytes % sector_size) != 0)) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	sectors = bytes / sector_size;
	while (done < bytes) {
		uint32_t chunk_sectors = sectors;
		uint32_t chunk_bytes;

		if (chunk_sectors > (EMMC_MAX_TRANS_BYTES / sector_size)) {
			chunk_sectors = EMMC_MAX_TRANS_BYTES / sector_size;
		}
		chunk_bytes = chunk_sectors * sector_size;

		memcpy(emmc_bounce, src + done, chunk_bytes);
		if (kStatus_Success != MMC_WriteBlocks(&emmc_sdmmc.card,
						       (const uint8_t *)emmc_bounce,
						       (uint32_t)block,
						       chunk_sectors)) {
			return ARM_DRIVER_ERROR;
		}

		block += chunk_sectors;
		done += chunk_bytes;
		sectors -= chunk_sectors;
	}

	return ARM_DRIVER_OK;
}

static int32_t emmc_flush(uint32_t unit)
{
	(void)unit;

	if (!emmc_ready) {
		return ARM_DRIVER_ERROR;
	}

	/* flush the eMMC internal write cache through the device (CMD6) */
	if (kStatus_Success != MMC_FlushCache(&emmc_sdmmc.card)) {
		return ARM_DRIVER_ERROR;
	}

	return ARM_DRIVER_OK;
}

static ARM_DRIVER_VERSION emmc_get_version(void)
{
	return (ARM_DRIVER_VERSION){ .api = BLKDEV_API_VERSION, .drv = 0x0100 };
}

static int32_t emmc_get_capabilities(uint32_t unit, BLKDEV_CAPABILITIES *caps)
{
	(void)unit;

	return sdmmc_storage_caps(caps) ? ARM_DRIVER_OK : ARM_DRIVER_ERROR;
}

/* Bound by the FatFs adapter's registration point (blkdev.c) as "emmc0". */
const ARM_DRIVER_BLKDEV blkdev_emmc = {
	.GetVersion = emmc_get_version,
	.GetCapabilities = emmc_get_capabilities,
	.Read = emmc_read,
	.Write = emmc_write,
	.Flush = emmc_flush,
};
