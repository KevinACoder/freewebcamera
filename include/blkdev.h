/*
 * @file   blkdev.h
 * @brief  Block device abstraction - one ops struct, one registry.
 *
 * This is the layer between the storage drivers (SATA/AHCI, NVMe) and every
 * consumer of raw blocks (FatFs today, a file-serving path or a disk tooling
 * command tomorrow). CMSIS-Driver has Driver_Storage.h for generic storage
 * but no shape for "an LBA-addressed device with removable-ish geometry",
 * and the two controllers here disagree on almost everything below the
 * surface (ATA FIS/PRDT versus NVMe queues); this header is the small common
 * vocabulary they are reduced to.
 *
 * The idiom is the project's usual one (DESIGN D11/D13): an ops struct of
 * function pointers plus ARM_DRIVER_VERSION, dispatch through the ops, and a
 * registry instead of compile-time wiring - so which drivers feed which
 * logical volume is a registration decision, not an #ifdef.
 *
 * CONTRACT:
 *  - Calls are task-context only, and may block: a SATA command polls to
 *    completion and an NVMe command waits for its completion interrupt.
 *  - `lba` is in units of the device's own sector size (see
 *    BLKDEV_CAPABILITIES.sector_size), `bytes` is a byte count that may span
 *    many sectors. Both drivers split a request into whatever their command
 *    engine can carry (64 KiB for ATA DMA EXT, 8 KiB for one NVMe PRP pair).
 *  - The caller owns every buffer; the driver neither frees nor retains it.
 *  - A unit is writable unless BLKDEV_CAPABILITIES.writable says otherwise;
 *    callers must check rather than rely on the write failing.
 *  - Return values are CMSIS codes: ARM_DRIVER_OK, or a negative
 *    ARM_DRIVER_ERROR* (the same vocabulary the CMSIS driver layer uses).
 */

#ifndef FREEWEBCAMERA_BLKDEV_H
#define FREEWEBCAMERA_BLKDEV_H

#include <stdbool.h>
#include <stdint.h>

#include "Driver_Common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLKDEV_API_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(0, 1)

typedef struct _BLKDEV_CAPABILITIES {
	uint64_t sector_count;	/* total addressable sectors */
	uint32_t sector_size;	/* bytes per logical sector (the LBA unit) */
	uint32_t max_transfer;	/* largest single request the driver accepts */
	uint32_t erase_block_size;	/* allocation unit, 0 when unknown */
	uint8_t writable;
	uint8_t media_present;	/* 0 = no medium (fixed disks: always 1) */
} BLKDEV_CAPABILITIES;

/* One driver can offer several units (two SATA ports, two NVMe namespaces),
 * so every call names the unit. The registry decides which units become
 * logical volumes and in what order. */
typedef struct _ARM_DRIVER_BLKDEV {
	ARM_DRIVER_VERSION (*GetVersion)(void);
	int32_t (*GetCapabilities)(uint32_t unit, BLKDEV_CAPABILITIES *caps);
	int32_t (*Read)(uint32_t unit, uint64_t lba, void *data, uint32_t bytes);
	int32_t (*Write)(uint32_t unit, uint64_t lba, const void *data,
			 uint32_t bytes);
	/* Push any cache inside the device. A hint, as in FatFs's CTRL_SYNC:
	 * a device with no write cache may answer OK without doing anything. */
	int32_t (*Flush)(uint32_t unit);
} ARM_DRIVER_BLKDEV;

/* --- registration (driver/binder side) ------------------------------------- */

/* Offer one unit of a driver to the registry. Returns the logical device
 * index, or a negative ARM_DRIVER_ERROR*. The registry copies nothing but the
 * pointer and the name, so both must outlive the call (static data). */
int32_t blkdev_register(const ARM_DRIVER_BLKDEV *driver, uint32_t unit,
			const char *name);

/* --- consumption (FatFs diskio and any other reader) ----------------------- */

uint32_t blkdev_count(void);

/* Name of a registered device ("sata0", "nvme0"), for reporting. NULL when
 * the index is out of range. */
const char *blkdev_name(uint32_t index);

int32_t blkdev_get_capabilities(uint32_t index, BLKDEV_CAPABILITIES *caps);
int32_t blkdev_read(uint32_t index, uint64_t lba, void *data, uint32_t bytes);
int32_t blkdev_write(uint32_t index, uint64_t lba, const void *data,
		     uint32_t bytes);
int32_t blkdev_flush(uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_BLKDEV_H */