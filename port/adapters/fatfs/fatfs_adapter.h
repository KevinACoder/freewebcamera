/*
 * @file   fatfs_adapter.h
 * @brief  Internal to port/adapters/fatfs/: what the pieces of this adapter
 *         say to each other. Not an interface.
 *
 * The interface the rest of the project sees is include/fs.h; the driver
 * handles this adapter binds are named in blkdev.c, which is the single
 * registration point (DESIGN D11/K3) exactly as the lwIP adapter's eth_ports
 * table is for the two MACs.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_FATFS_ADAPTER_H
#define FREEWEBCAMERA_FATFS_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Offer the board's block devices to the registry: the two SATA ports through
 * ARM_DRIVER_AHCI and the NVMe controller through ARM_DRIVER_NVME. Presence is
 * decided by the drivers (Initialize() probes), not by a compile-time list.
 * Idempotent. Returns 0 when at least one device was bound. */
int fatfs_bind_devices(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_FATFS_ADAPTER_H */