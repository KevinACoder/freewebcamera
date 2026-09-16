/*
 * @file   diskio.c
 * @brief  FatFs's physical drive layer: a logical volume is a block device.
 *
 * This is the other half of the integration (the first half is ffconf.h): FatFs
 * asks for sectors by physical drive number and we translate to
 * include/blkdev.h, which is where the ATA command engine and the NVMe queue
 * pair stop being distinguishable.
 *
 * Volume numbering is the block layer's: logical volume n is block device n
 * (include/fs.h says the same thing). VolToPart below maps every volume to
 * partition 1 of its own physical drive, which is what makes f_fdisk/f_mkfs
 * addressable per volume and what makes a disk formatted by this integration
 * mountable again.
 *
 * Note what is NOT here: no cache, no bounce buffer, no alignment handling.
 * The drivers own that (an ATA transfer goes through a page-aligned bounce
 * buffer, an NVMe transfer through PRP1/PRP2 or a bounce), and duplicating it
 * would only add a place for the two copies to disagree.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include "ff.h"
#include "diskio.h"

#include <stdint.h>

#include "blkdev.h"

_Static_assert(FF_VOLUMES >= 3,
	       "the board's three block devices need three logical volumes");

/* With FF_MULTI_PARTITION, FatFs consults this table to turn a logical volume
 * into (physical drive, partition). One drive per volume, partition 1.
 *
 * Not const: ff.h declares it as `extern PARTITION VolToPart[]`, and matching
 * the vendored declaration is worth more than the qualifier. */
PARTITION VolToPart[FF_VOLUMES] = {
	{ 0, 0 },
	{ 1, 0 },
	{ 2, 0 },
};

/* What disk_status() reports. FatFs polls this, so it is kept rather than
 * re-deriving it (which would mean touching the device on every status call). */
static DSTATUS disk_state[FF_VOLUMES];

DSTATUS disk_status(BYTE pdrv)
{
	if (pdrv >= FF_VOLUMES) {
		return STA_NOINIT | STA_NODISK;
	}

	return disk_state[pdrv];
}

DSTATUS disk_initialize(BYTE pdrv)
{
	BLKDEV_CAPABILITIES caps;

	if (pdrv >= FF_VOLUMES) {
		return STA_NOINIT | STA_NODISK;
	}

	/* The device is already up: it was probed when it was bound (the
	 * binding calls the driver's Initialize). What is left is to report
	 * whether it can be used. */
	if (blkdev_get_capabilities(pdrv, &caps) != ARM_DRIVER_OK) {
		disk_state[pdrv] = STA_NOINIT | STA_NODISK;
		return disk_state[pdrv];
	}

	disk_state[pdrv] = 0;
	if (!caps.media_present) {
		disk_state[pdrv] |= STA_NODISK | STA_NOINIT;
	}
	if (!caps.writable) {
		disk_state[pdrv] |= STA_PROTECT;
	}

	return disk_state[pdrv];
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	BLKDEV_CAPABILITIES caps;

	if (pdrv >= FF_VOLUMES || (disk_state[pdrv] & STA_NOINIT)) {
		return RES_NOTRDY;
	}
	if (blkdev_get_capabilities(pdrv, &caps) != ARM_DRIVER_OK) {
		return RES_ERROR;
	}

	/* The whole request goes down as one call: both drivers split it into
	 * command-sized chunks themselves, and doing it here as well would only
	 * mean two places to keep in agreement. */
	if (blkdev_read(pdrv, (uint64_t)sector, buff,
			(uint32_t)count * caps.sector_size) != ARM_DRIVER_OK) {
		return RES_ERROR;
	}

	return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	BLKDEV_CAPABILITIES caps;

	if (pdrv >= FF_VOLUMES || (disk_state[pdrv] & STA_NOINIT)) {
		return RES_NOTRDY;
	}
	if (disk_state[pdrv] & STA_PROTECT) {
		return RES_WRPRT;
	}
	if (blkdev_get_capabilities(pdrv, &caps) != ARM_DRIVER_OK) {
		return RES_ERROR;
	}

	if (blkdev_write(pdrv, (uint64_t)sector, buff,
			 (uint32_t)count * caps.sector_size) != ARM_DRIVER_OK) {
		return RES_ERROR;
	}

	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	BLKDEV_CAPABILITIES caps;

	if (pdrv >= FF_VOLUMES) {
		return RES_PARERR;
	}

	switch (cmd) {
	case CTRL_SYNC:
		/* A hint, as FatFs documents it: a device with no write cache
		 * may answer OK without doing anything. Both of ours issue a
		 * real flush command. */
		return (blkdev_flush(pdrv) == ARM_DRIVER_OK) ? RES_OK : RES_ERROR;

	case GET_SECTOR_COUNT:
		if (blkdev_get_capabilities(pdrv, &caps) != ARM_DRIVER_OK) {
			return RES_ERROR;
		}
		/* The interface reports 64-bit sectors; this build addresses
		 * with 32 (FF_LBA64 == 0), far more than these disks need. */
		*(LBA_t *)buff = (caps.sector_count > 0xffffffffULL)
					 ? (LBA_t)0xffffffff
					 : (LBA_t)caps.sector_count;
		return RES_OK;

	case GET_SECTOR_SIZE:
		if (blkdev_get_capabilities(pdrv, &caps) != ARM_DRIVER_OK) {
			return RES_ERROR;
		}
		*(WORD *)buff = (WORD)caps.sector_size;
		return RES_OK;

	case GET_BLOCK_SIZE:
		if (blkdev_get_capabilities(pdrv, &caps) != ARM_DRIVER_OK) {
			return RES_ERROR;
		}
		/* Erase block size in sectors; 1 (unknown) is the documented
		 * way to say "no opinion" and is what f_mkfs aligns against. */
		*(DWORD *)buff = (caps.erase_block_size != 0)
					 ? caps.erase_block_size
					 : 1;
		return RES_OK;

	default:
		return RES_PARERR;
	}
}

/* FatFs asks for the current time only when it stamps a file and only when
 * FF_FS_NORTC == 0. No RTC is ported, so this build uses the fixed timestamp
 * instead - the function is deliberately absent rather than returning a lie. */
#if FF_FS_NORTC == 0
#error "no RTC is ported: set FF_FS_NORTC to 1 or implement get_fattime()"
#endif