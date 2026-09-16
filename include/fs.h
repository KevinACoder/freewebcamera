/*
 * @file   fs.h
 * @brief  Project-owned interface: the file system over the block devices.
 *
 * The implementation is port/adapters/fatfs/ (vendored FatFs R0.16 plus the
 * diskio binding). Nothing of FatFs is visible here - not ff.h, not the drive
 * numbering, not the mount state - so swapping the file system library is a
 * change to one adapter, not to the application. Same shape as shell.h and
 * net.h, and for the same reason.
 *
 * Volumes are numbered the way the block layer numbers its devices: volume n
 * is block device n (see include/blkdev.h). fs_start() binds whatever the
 * storage drivers registered; a volume with no FAT filesystem on it simply
 * does not mount, which is a reportable state, not an error in the binding.
 *
 * CONTRACT:
 *  - fs_start() is idempotent and callable from a task only (mounting reads
 *    blocks, which blocks). It returns 0 once the binding exists, whether or
 *    not any volume could be mounted.
 *  - fs_mount/fs_umount take a volume index; out-of-range is a parameter
 *    error. Mounting a volume that already is mounted is a no-op.
 */

#ifndef FREEWEBCAMERA_FS_H
#define FREEWEBCAMERA_FS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the block devices to the file system and mount what it can. */
int fs_start(void);

/* Number of volumes the file system layer knows about (bound block devices). */
uint32_t fs_volume_count(void);

/* Mount/unmount one volume. Returns 0 on success, non-zero on failure. */
int fs_mount(uint32_t volume);
int fs_umount(uint32_t volume);

bool fs_is_mounted(uint32_t volume);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_FS_H */