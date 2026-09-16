/*
 * @file   fatfs_adapter.c
 * @brief  The file-system layer: binds the block devices, mounts what it can,
 *         and implements include/fs.h - the only thing the application sees.
 *
 * Mounting is where this adapter's decisions show up, so they are stated here:
 *
 *   - A volume that cannot be mounted is not an error of the binding. The
 *     three disks arrive from the lab with whatever was on them (MBR, GPT,
 *     NetBSD wedges, or nothing), and "no FAT volume here" is a fact to
 *     report, not a failure to hide. fs_start() still returns 0.
 *   - The mapping from volume to device is the block layer's: volume n is
 *     block device n, and this file is the only place that pairing is
 *     interpreted as a FAT drive ("0:", "1:", ...).
 *   - FatFs keeps mount state in the FATFS object; whether a volume is mounted
 *     is tracked here, because FatFs has no query for it and asking by
 *     attempting a mount would touch the disk.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <stdio.h>

#include "board.h"
#include "blkdev.h"
#include "cmsis_os2.h"
#include "ff.h"
#include "fs.h"

#include "fatfs_adapter.h"

static FATFS volume_fs[FF_VOLUMES];
static uint8_t volume_mounted[FF_VOLUMES];
static int fs_started;

/* FatFs's logical drive name. FF_STR_VOLUME_ID == 0 makes it the number. */
static void volume_path(uint32_t volume, char *out, unsigned int size)
{
	(void)snprintf(out, size, "%u:", volume);
}

uint32_t fs_volume_count(void)
{
	uint32_t count = blkdev_count();

	return (count > FF_VOLUMES) ? FF_VOLUMES : count;
}

bool fs_is_mounted(uint32_t volume)
{
	if (volume >= FF_VOLUMES) {
		return false;
	}

	return volume_mounted[volume] != 0;
}

int fs_mount(uint32_t volume)
{
	char path[4];
	FRESULT fr;

	if (volume >= fs_volume_count()) {
		return FR_INVALID_DRIVE;
	}
	if (volume_mounted[volume]) {
		return FR_OK;
	}

	volume_path(volume, path, sizeof(path));
	fr = f_mount(&volume_fs[volume], path, 1);
	if (fr == FR_OK) {
		volume_mounted[volume] = 1;
	}

	return (int)fr;
}

int fs_umount(uint32_t volume)
{
	char path[4];
	FRESULT fr;

	if (volume >= FF_VOLUMES) {
		return FR_INVALID_DRIVE;
	}
	if (!volume_mounted[volume]) {
		return FR_OK;
	}

	volume_path(volume, path, sizeof(path));
	/* opt == 0 unregisters the work area and flushes the volume. */
	fr = f_mount(NULL, path, 0);
	if (fr == FR_OK) {
		volume_mounted[volume] = 0;
	}

	return (int)fr;
}

static const char *fat_type(BYTE fs_type)
{
	switch (fs_type) {
	case FS_FAT12:
		return "FAT12";
	case FS_FAT16:
		return "FAT16";
	case FS_FAT32:
		return "FAT32";
	default:
		return "FAT?";
	}
}

/* One line per mounted volume: what it is and how much room is left. The
 * numbers come from the filesystem itself (f_getfree), not from the block
 * device's size - the difference between the two is what tells an operator
 * that the volume does not cover the whole disk. */
static void report_mounted(uint32_t volume)
{
	FATFS *fs = &volume_fs[volume];
	DWORD free_clusters = 0;
	char path[4];
	uint64_t total_mb;
	uint64_t free_mb;

	volume_path(volume, path, sizeof(path));

	if (f_getfree(path, &free_clusters, &fs) != FR_OK) {
		board_log("fs: %s mounted (%s)\n", blkdev_name(volume),
			  fat_type(volume_fs[volume].fs_type));
		return;
	}

	total_mb = ((uint64_t)(fs->n_fatent - 2) * fs->csize) / 2048ULL;
	free_mb = ((uint64_t)free_clusters * fs->csize) / 2048ULL;

	board_log("fs: %s mounted, %s, %llu MB total, %llu MB free\n",
		  blkdev_name(volume), fat_type(fs->fs_type),
		  (unsigned long long)total_mb, (unsigned long long)free_mb);
}

int fs_start(void)
{
	uint32_t volume;
	uint32_t count;

	if (fs_started) {
		return 0;
	}
	fs_started = 1;

	if (fatfs_bind_devices() != 0) {
		board_log("fs: no block device available\n");
		return -1;
	}

	count = fs_volume_count();
	board_log("fs: %u block device(s) bound\n", (unsigned)count);

	for (volume = 0; volume < count; volume++) {
		int fr = fs_mount(volume);

		if (fr == FR_OK) {
			report_mounted(volume);
		} else {
			board_log("fs: %s has no mountable FAT volume (FR_%d)\n",
				  blkdev_name(volume), fr);
		}
	}

	return 0;
}