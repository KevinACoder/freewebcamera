/*
 * @file   blkdev.c
 * @brief  The block device registry, and the one place where this board's
 *         storage drivers are named.
 *
 * Two jobs, both deliberately in one file:
 *
 *   - it implements include/blkdev.h's registry (a table of ops + unit +
 *     name, filled at run time - DESIGN D11's "op struct plus a single
 *     registration point", not a compile-time device list);
 *   - it is that registration point. The SATA ports and the NVMe controller
 *     are adapted to the common block-device ops here, so nothing above this
 *     file ever knows whether a volume is an ATA device behind a command
 *     engine or an NVMe namespace behind a queue pair. Swapping a driver is a
 *     change to the table below.
 *
 * The driver handles are declared by hand rather than by including a driver's
 * header: the same rule the lwIP adapter follows for Driver_ETH_MAC0/1. What
 * crosses this boundary is the frozen CMSIS interface, nothing else.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <stdio.h>

#include "Driver_Common.h"
#include "ahci.h"
#include "blkdev.h"
#include "nvme.h"

#include "fatfs_adapter.h"

/* Storage devices this board can have bound at once: 2 SATA + 1 NVMe, with
 * room for a USB mass-storage device later (M6). */
#define BLKDEV_MAX 8

/* The driver instances. Provided by drivers/rk3568_sata.c and
 * drivers/dwc_nvme.c; nothing above drivers/ includes their headers. */
extern ARM_DRIVER_AHCI Driver_AHCI;
extern ARM_DRIVER_NVME Driver_NVME;

struct blkdev_entry {
	const ARM_DRIVER_BLKDEV *driver;
	uint32_t unit;
	const char *name;
};

static struct blkdev_entry devices[BLKDEV_MAX];
static uint32_t device_count;

/* --- the SATA side: an AHCI port is one block device --------------------- */

static int32_t sata_capabilities(uint32_t unit, BLKDEV_CAPABILITIES *caps)
{
	AHCI_CAPABILITIES ahci = Driver_AHCI.GetCapabilities(unit);

	if (ahci.sector_count == 0 || ahci.sector_size == 0) {
		return ARM_DRIVER_ERROR;
	}

	caps->sector_count = ahci.sector_count;
	caps->sector_size = ahci.sector_size;
	caps->max_transfer = ahci.max_transfer;
	caps->erase_block_size = 0;	/* not reported by the ATA path */
	caps->writable = 1;
	caps->media_present = 1;

	return ARM_DRIVER_OK;
}

static int32_t sata_read(uint32_t unit, uint64_t lba, void *data, uint32_t bytes)
{
	BLKDEV_CAPABILITIES caps;

	if (sata_capabilities(unit, &caps) != ARM_DRIVER_OK ||
	    (bytes % caps.sector_size) != 0) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return Driver_AHCI.ReadBlocks(unit, lba, data, bytes / caps.sector_size);
}

static int32_t sata_write(uint32_t unit, uint64_t lba, const void *data,
			  uint32_t bytes)
{
	BLKDEV_CAPABILITIES caps;

	if (sata_capabilities(unit, &caps) != ARM_DRIVER_OK ||
	    (bytes % caps.sector_size) != 0) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return Driver_AHCI.WriteBlocks(unit, lba, data, bytes / caps.sector_size);
}

static int32_t sata_flush(uint32_t unit)
{
	return Driver_AHCI.Flush(unit);
}

/* --- the NVMe side: a controller is one block device --------------------- */

static int32_t nvme_capabilities(uint32_t unit, BLKDEV_CAPABILITIES *caps)
{
	NVME_CAPABILITIES nvme = Driver_NVME.GetCapabilities(unit);

	if (nvme.sector_count == 0 || nvme.sector_size == 0) {
		return ARM_DRIVER_ERROR;
	}

	caps->sector_count = nvme.sector_count;
	caps->sector_size = nvme.sector_size;
	caps->max_transfer = nvme.max_transfer;
	caps->erase_block_size = 0;
	caps->writable = 1;
	caps->media_present = 1;

	return ARM_DRIVER_OK;
}

static int32_t nvme_read(uint32_t unit, uint64_t lba, void *data, uint32_t bytes)
{
	BLKDEV_CAPABILITIES caps;

	if (nvme_capabilities(unit, &caps) != ARM_DRIVER_OK ||
	    (bytes % caps.sector_size) != 0) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return Driver_NVME.ReadBlocks(unit, lba, data, bytes / caps.sector_size);
}

static int32_t nvme_write(uint32_t unit, uint64_t lba, const void *data,
			  uint32_t bytes)
{
	BLKDEV_CAPABILITIES caps;

	if (nvme_capabilities(unit, &caps) != ARM_DRIVER_OK ||
	    (bytes % caps.sector_size) != 0) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return Driver_NVME.WriteBlocks(unit, lba, data, bytes / caps.sector_size);
}

static int32_t nvme_flush(uint32_t unit)
{
	return Driver_NVME.Flush(unit);
}

/* --- the two ops sets ---------------------------------------------------- */

static ARM_DRIVER_VERSION blkdev_get_version(void)
{
	return (ARM_DRIVER_VERSION){ .api = BLKDEV_API_VERSION, .drv = 0x0100 };
}

static const ARM_DRIVER_BLKDEV blkdev_sata = {
	.GetVersion = blkdev_get_version,
	.GetCapabilities = sata_capabilities,
	.Read = sata_read,
	.Write = sata_write,
	.Flush = sata_flush,
};

static const ARM_DRIVER_BLKDEV blkdev_nvme = {
	.GetVersion = blkdev_get_version,
	.GetCapabilities = nvme_capabilities,
	.Read = nvme_read,
	.Write = nvme_write,
	.Flush = nvme_flush,
};

/* --- registry (include/blkdev.h) ----------------------------------------- */

int32_t blkdev_register(const ARM_DRIVER_BLKDEV *driver, uint32_t unit,
			const char *name)
{
	if (driver == NULL || name == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}
	if (device_count >= BLKDEV_MAX) {
		return ARM_DRIVER_ERROR;
	}

	devices[device_count].driver = driver;
	devices[device_count].unit = unit;
	devices[device_count].name = name;
	device_count++;

	return (int32_t)device_count - 1;
}

uint32_t blkdev_count(void)
{
	return device_count;
}

const char *blkdev_name(uint32_t index)
{
	if (index >= device_count) {
		return NULL;
	}

	return devices[index].name;
}

int32_t blkdev_get_capabilities(uint32_t index, BLKDEV_CAPABILITIES *caps)
{
	if (index >= device_count || caps == NULL) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return devices[index].driver->GetCapabilities(devices[index].unit, caps);
}

int32_t blkdev_read(uint32_t index, uint64_t lba, void *data, uint32_t bytes)
{
	if (index >= device_count) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return devices[index].driver->Read(devices[index].unit, lba, data, bytes);
}

int32_t blkdev_write(uint32_t index, uint64_t lba, const void *data,
		     uint32_t bytes)
{
	if (index >= device_count) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return devices[index].driver->Write(devices[index].unit, lba, data, bytes);
}

int32_t blkdev_flush(uint32_t index)
{
	if (index >= device_count) {
		return ARM_DRIVER_ERROR_PARAMETER;
	}

	return devices[index].driver->Flush(devices[index].unit);
}

/* --- the registration point ---------------------------------------------- */

int fatfs_bind_devices(void)
{
	static const char *const sata_names[] = { "sata0", "sata1" };
	uint32_t i;

	if (device_count != 0) {
		return 0;	/* already bound */
	}

	/* SATA: two on-chip controllers, one port each. Initialize() is what
	 * decides presence - it takes the engine over, checks that firmware
	 * trained the link and reads the geometry; a port with no device
	 * reports an error and is simply not bound. */
	for (i = 0; i < sizeof(sata_names) / sizeof(sata_names[0]); i++) {
		if (Driver_AHCI.Initialize(i, NULL) != ARM_DRIVER_OK) {
			continue;
		}
		(void)blkdev_register(&blkdev_sata, i, sata_names[i]);
	}

	/* NVMe: the one controller behind pcie2x1. Initialize() brings the
	 * PCIe backend up (idempotent) and probes the endpoint. */
	if (Driver_NVME.Initialize(0, NULL) == ARM_DRIVER_OK) {
		(void)blkdev_register(&blkdev_nvme, 0, "nvme0");
	}

	if (device_count == 0) {
		return -1;
	}

	return 0;
}