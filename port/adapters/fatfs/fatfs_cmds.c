/*
 * @file   fatfs_cmds.c
 * @brief  Shell commands for the storage stack - the operational view the M3
 *         acceptance runs are written against.
 *
 *   stor                       devices: capacity, mount state
 *   stor mount|umount <n>      mount state of one volume
 *   stor ls <n> [dir]          directory listing
 *   stor wr <n> <path> <bytes> write a deterministic pattern, read it back,
 *                              verify: the write/read closure in one command
 *   stor rd <n> <path>         read a file and print size + checksum
 *   stor cat <n> <path>        print the first part of a text file
 *   stor find <n> <pattern>    f_findfirst over a volume
 *   stor fdisk <n> yes         write an MBR: one partition, the whole disk
 *   stor mkfs <n> yes          format that partition as FAT
 *
 * fdisk/mkfs are destructive by nature and this board's disks carry whatever
 * the lab left on them, so they take an explicit confirmation word rather than
 * a flag: a typo cannot reformat a disk.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blkdev.h"
#include "cmsis_os2.h"
#include "ff.h"
#include "fs.h"

#include "cherrysh_adapter.h"
#include "csh.h"

/* One shared I/O buffer for every command here. 64 KiB is the largest ATA
 * transfer the SATA driver's command engine can carry in one PRD, so a chunk
 * this size never makes the driver split a request in the middle. */
#define STOR_BUF_SIZE (64 * 1024)
static uint8_t stor_buf[STOR_BUF_SIZE] __attribute__((aligned(64)));

/* f_mkfs/f_fdisk working buffer: one sector is enough by the specification,
 * the page is for the alignment those paths like. */
static uint8_t stor_work[4096] __attribute__((aligned(4096)));

static void usage(chry_shell_t *csh)
{
	csh_printf(csh, "usage: stor [mount|umount|ls|rd|cat|wr|find|fdisk|mkfs] ...\r\n");
	csh_printf(csh, "       stor                     list devices\r\n");
	csh_printf(csh, "       stor mount|umount <n>\r\n");
	csh_printf(csh, "       stor ls <n> [dir]\r\n");
	csh_printf(csh, "       stor rd <n> <path>       file read + checksum\r\n");
	csh_printf(csh, "       stor cat <n> <path>      first 2 KiB as text\r\n");
	csh_printf(csh, "       stor wr <n> <path> <bytes>   write, read back, verify\r\n");
	csh_printf(csh, "       stor find <n> <pattern>\r\n");
	csh_printf(csh, "       stor fdisk <n> yes       MBR: one partition, whole disk\r\n");
	csh_printf(csh, "       stor mkfs <n> yes        format as FAT\r\n");
}

/* "n:" plus the caller's path; the caller passes paths without the drive. */
static void full_path(uint32_t volume, const char *arg, char *out,
		      unsigned int size)
{
	(void)snprintf(out, size, "%u:%s", volume,
		       (arg != NULL && arg[0] != '\0') ? arg : "/");
}

static int parse_volume(const char *arg, uint32_t *volume)
{
	unsigned int value;

	if (arg == NULL || arg[0] < '0' || arg[0] > '9' || arg[1] != '\0') {
		return -1;
	}

	value = (unsigned int)(arg[0] - '0');
	if (value >= fs_volume_count()) {
		return -1;
	}
	*volume = value;

	return 0;
}

static uint32_t fnv1a(const uint8_t *data, uint32_t len, uint32_t hash)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}

	return hash;
}

/* The deterministic byte pattern the write/verify command uses: a plain LCG,
 * so the reader can regenerate it without storing it anywhere. */
static uint8_t pattern_byte(uint32_t index)
{
	uint32_t seed = index * 2654435761u + 12345u;

	return (uint8_t)(seed >> 16);
}

/* Decimal only, and small: the command's byte counts are file sizes a person
 * types, not machine output. */
static int parse_u32(const char *arg, uint32_t *value)
{
	uint32_t v = 0;

	if (arg == NULL || arg[0] == '\0') {
		return -1;
	}

	while (*arg != '\0') {
		if (*arg < '0' || *arg > '9') {
			return -1;
		}
		v = v * 10u + (uint32_t)(*arg - '0');
		arg++;
	}
	*value = v;

	return 0;
}

/* --- stor (list) ---------------------------------------------------------- */

static void list_devices(chry_shell_t *csh)
{
	uint32_t i;

	for (i = 0; i < fs_volume_count(); i++) {
		BLKDEV_CAPABILITIES caps;
		const char *name = blkdev_name(i);
		uint64_t mb = 0;

		if (blkdev_get_capabilities(i, &caps) == ARM_DRIVER_OK) {
			mb = caps.sector_count * caps.sector_size / (1024ULL * 1024ULL);
		}

		if (fs_is_mounted(i)) {
			csh_printf(csh, "%u %-6s %6llu MB  mounted\r\n", i,
				   (name != NULL) ? name : "?", (unsigned long long)mb);
		} else {
			csh_printf(csh, "%u %-6s %6llu MB  not mounted\r\n", i,
				   (name != NULL) ? name : "?", (unsigned long long)mb);
		}
	}
}

/* --- ls ------------------------------------------------------------------- */

static void cmd_ls(chry_shell_t *csh, uint32_t volume, const char *dir)
{
	DIR dj;
	FILINFO info;
	char path[64];
	FRESULT fr;
	uint32_t entries = 0;

	full_path(volume, dir, path, sizeof(path));

	fr = f_opendir(&dj, path);
	if (fr != FR_OK) {
		csh_printf(csh, "stor: cannot open %s (FR_%d)\r\n", path, (int)fr);
		return;
	}

	for (;;) {
		fr = f_readdir(&dj, &info);
		if (fr != FR_OK || info.fname[0] == '\0') {
			break;
		}

		if (info.fattrib & AM_DIR) {
			csh_printf(csh, "  <dir>            %s\r\n", info.fname);
		} else {
			csh_printf(csh, "  %10llu       %s\r\n",
				   (unsigned long long)info.fsize, info.fname);
		}
		entries++;
	}
	(void)f_closedir(&dj);

	csh_printf(csh, "%s: %u entr%s, FR_%d\r\n", path, (unsigned)entries,
		   (entries == 1) ? "y" : "ies", (int)fr);
}

/* --- rd (read + checksum) -------------------------------------------------- */

static void cmd_rd(chry_shell_t *csh, uint32_t volume, const char *file)
{
	FIL fp;
	char path[64];
	FRESULT fr;
	uint32_t total = 0;
	uint32_t hash = 2166136261u;
	uint32_t start_ms;
	uint32_t elapsed_ms;

	full_path(volume, file, path, sizeof(path));

	fr = f_open(&fp, path, FA_READ);
	if (fr != FR_OK) {
		csh_printf(csh, "stor: cannot open %s (FR_%d)\r\n", path, (int)fr);
		return;
	}

	start_ms = osKernelGetTickCount();
	for (;;) {
		UINT got = 0;

		fr = f_read(&fp, stor_buf, STOR_BUF_SIZE, &got);
		if (fr != FR_OK || got == 0) {
			break;
		}
		hash = fnv1a(stor_buf, got, hash);
		total += got;
	}
	elapsed_ms = (osKernelGetTickCount() - start_ms) + 1u;
	(void)f_close(&fp);

	csh_printf(csh,
		   "%s: %lu bytes, fnv1a %08lx, %lu ms (%lu KiB/s), FR_%d\r\n",
		   path, (unsigned long)total, (unsigned long)hash,
		   (unsigned long)elapsed_ms,
		   (unsigned long)((total / 1024u) * 1000u / elapsed_ms), (int)fr);
}

/* --- cat (text) ----------------------------------------------------------- */

static void cmd_cat(chry_shell_t *csh, uint32_t volume, const char *file)
{
	FIL fp;
	char path[64];
	FRESULT fr;
	UINT got = 0;
	UINT i;
	UINT shown;

	full_path(volume, file, path, sizeof(path));

	fr = f_open(&fp, path, FA_READ);
	if (fr != FR_OK) {
		csh_printf(csh, "stor: cannot open %s (FR_%d)\r\n", path, (int)fr);
		return;
	}

	fr = f_read(&fp, stor_buf, 2048, &got);
	shown = got;
	if (fr == FR_OK && got > 0) {
		for (i = 0; i < got; i++) {
			uint8_t c = stor_buf[i];

			if (c == '\r') {
				continue;
			}
			if (c == '\n' || (c >= 0x20 && c < 0x7f)) {
				csh_printf(csh, "%c", c);
			} else {
				csh_printf(csh, ".");
			}
		}
		if (stor_buf[got - 1] != '\n') {
			csh_printf(csh, "\r\n");
		}
	}
	csh_printf(csh, "[%s: %lu bytes shown, file is %lu bytes, FR_%d]\r\n", path,
		   (unsigned long)shown, (unsigned long)f_size(&fp), (int)fr);
	(void)f_close(&fp);
}

/* --- wr (write, read back, verify) ---------------------------------------- */

static void cmd_wr(chry_shell_t *csh, uint32_t volume, const char *file,
		   uint32_t bytes)
{
	FIL fp;
	char path[64];
	FRESULT fr;
	uint32_t done = 0;
	uint32_t start_ms;
	uint32_t elapsed_ms;
	int mismatch = -1;

	full_path(volume, file, path, sizeof(path));

	fr = f_open(&fp, path, FA_CREATE_ALWAYS | FA_WRITE);
	if (fr != FR_OK) {
		csh_printf(csh, "stor: cannot create %s (FR_%d)\r\n", path, (int)fr);
		return;
	}

	start_ms = osKernelGetTickCount();
	while (done < bytes) {
		uint32_t chunk = bytes - done;
		UINT wrote = 0;
		uint32_t i;

		if (chunk > STOR_BUF_SIZE) {
			chunk = STOR_BUF_SIZE;
		}
		for (i = 0; i < chunk; i++) {
			stor_buf[i] = pattern_byte(done + i);
		}

		fr = f_write(&fp, stor_buf, chunk, &wrote);
		if (fr != FR_OK || wrote != chunk) {
			break;
		}
		done += chunk;
	}
	fr = f_close(&fp);	/* close flushes; fails here are a real error */
	if (fr != FR_OK || done != bytes) {
		csh_printf(csh, "stor: write failed after %lu bytes (FR_%d)\r\n",
			   (unsigned long)done, (int)fr);
		(void)f_unlink(path);
		return;
	}
	elapsed_ms = (osKernelGetTickCount() - start_ms) + 1u;

	/* Read it back through the same path a reader would take, and compare
	 * against the pattern - a write that "succeeded" but landed elsewhere
	 * passes a status check and fails this. */
	fr = f_open(&fp, path, FA_READ);
	if (fr != FR_OK) {
		csh_printf(csh, "stor: reopen failed (FR_%d)\r\n", (int)fr);
		return;
	}

	done = 0;
	mismatch = -1;
	fr = FR_OK;
	while (done < bytes) {
		uint32_t chunk = bytes - done;
		UINT got = 0;
		uint32_t i;

		if (chunk > STOR_BUF_SIZE) {
			chunk = STOR_BUF_SIZE;
		}
		fr = f_read(&fp, stor_buf, chunk, &got);
		if (fr != FR_OK || got != chunk) {
			break;
		}
		for (i = 0; i < chunk; i++) {
			if (stor_buf[i] != pattern_byte(done + i)) {
				mismatch = (int)(done + i);
				break;
			}
		}
		if (mismatch >= 0) {
			break;
		}
		done += chunk;
	}
	(void)f_close(&fp);

	if (fr != FR_OK) {
		csh_printf(csh, "stor: read-back failed at %lu bytes (FR_%d)\r\n",
			   (unsigned long)done, (int)fr);
	} else if (mismatch >= 0) {
		csh_printf(csh, "stor: MISMATCH at byte %d\r\n", mismatch);
	} else if (done != bytes) {
		csh_printf(csh, "stor: short read-back, %lu of %lu bytes\r\n",
			   (unsigned long)done, (unsigned long)bytes);
	} else {
		csh_printf(csh,
			   "%s: %lu bytes written and verified, %lu ms (%lu KiB/s)\r\n",
			   path, (unsigned long)bytes, (unsigned long)elapsed_ms,
			   (unsigned long)((bytes / 1024u) * 1000u / elapsed_ms));
	}
}

/* --- find ----------------------------------------------------------------- */

static void cmd_find(chry_shell_t *csh, uint32_t volume, const char *pattern)
{
	DIR dj;
	FILINFO info;
	char path[64];
	FRESULT fr;
	uint32_t hits = 0;

	(void)snprintf(path, sizeof(path), "%u:/%s", volume, pattern);

	fr = f_findfirst(&dj, &info, path, pattern);
	while (fr == FR_OK && info.fname[0] != '\0') {
		csh_printf(csh, "  %s\r\n", info.fname);
		hits++;
		fr = f_findnext(&dj, &info);
	}
	(void)f_closedir(&dj);

	csh_printf(csh, "%s: %u match(es), FR_%d\r\n", path, (unsigned)hits, (int)fr);
}

/* --- fdisk / mkfs --------------------------------------------------------- */

static void cmd_fdisk(chry_shell_t *csh, uint32_t volume)
{
	/* Percentages; 0 terminates. One partition covering the disk, which is
	 * what VolToPart in diskio.c expects to find. */
	static const LBA_t parts[4] = { 100, 0, 0, 0 };
	FRESULT fr;

	fr = f_fdisk((BYTE)volume, parts, stor_work);
	if (fr != FR_OK) {
		csh_printf(csh, "stor: fdisk %s failed (FR_%d)\r\n",
			   blkdev_name(volume), (int)fr);
		return;
	}

	/* The partition table was written behind FatFs's back: an already
	 * mounted volume would keep its stale view of the disk. */
	(void)fs_umount(volume);
	csh_printf(csh, "stor: %s: MBR written, one partition, whole disk\r\n",
		   blkdev_name(volume));
}

static void cmd_mkfs(chry_shell_t *csh, uint32_t volume)
{
	char path[4];
	FRESULT fr;

	(void)snprintf(path, sizeof(path), "%u:", volume);

	(void)fs_umount(volume);

	/* NULL for the parameters means FatFs's own defaults, which is
	 * FM_ANY + automatic allocation unit - and FM_ANY is what lets it pick
	 * FAT32 for a 119 GB volume. Passing a zeroed MKFS_PARM instead does
	 * NOT mean "defaults": fmt 0 is neither FM_FAT nor FM_FAT32 and
	 * f_mkfs rejects it with FR_INVALID_PARAMETER. */
	fr = f_mkfs(path, NULL, stor_work, sizeof(stor_work));
	if (fr != FR_OK) {
		csh_printf(csh, "stor: mkfs %s failed (FR_%d)\r\n",
			   blkdev_name(volume), (int)fr);
		return;
	}

	csh_printf(csh, "stor: %s formatted\r\n", blkdev_name(volume));
}

/* --- command entry -------------------------------------------------------- */

static int cmd_stor(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	const char *op;
	uint32_t volume;

	if (argc < 2) {
		list_devices(csh);
		return 0;
	}
	op = argv[1];

	if (strcmp(op, "mount") == 0 || strcmp(op, "umount") == 0) {
		int fr;

		if (argc < 3 || parse_volume(argv[2], &volume) != 0) {
			csh_printf(csh, "stor: volume must be 0..%u\r\n",
				   (unsigned)(fs_volume_count() - 1));
			return -1;
		}

		fr = (op[0] == 'm') ? fs_mount(volume) : fs_umount(volume);
		csh_printf(csh, "stor: %s %s: FR_%d\r\n", op, blkdev_name(volume), fr);
		return fr == FR_OK ? 0 : -1;
	}

	if (strcmp(op, "ls") == 0) {
		if (argc < 3 || parse_volume(argv[2], &volume) != 0) {
			usage(csh);
			return -1;
		}
		cmd_ls(csh, volume, (argc >= 4) ? argv[3] : NULL);
		return 0;
	}

	if (strcmp(op, "rd") == 0 || strcmp(op, "cat") == 0) {
		if (argc < 4 || parse_volume(argv[2], &volume) != 0) {
			usage(csh);
			return -1;
		}
		if (op[1] == 'd') {
			cmd_rd(csh, volume, argv[3]);
		} else {
			cmd_cat(csh, volume, argv[3]);
		}
		return 0;
	}

	if (strcmp(op, "wr") == 0) {
		uint32_t bytes;

		if (argc < 5 || parse_volume(argv[2], &volume) != 0) {
			usage(csh);
			return -1;
		}
		if (parse_u32(argv[4], &bytes) != 0 || bytes == 0) {
			csh_printf(csh, "stor: byte count must be a positive decimal"
					" number\r\n");
			return -1;
		}
		cmd_wr(csh, volume, argv[3], bytes);
		return 0;
	}

	if (strcmp(op, "find") == 0) {
		if (argc < 4 || parse_volume(argv[2], &volume) != 0) {
			usage(csh);
			return -1;
		}
		cmd_find(csh, volume, argv[3]);
		return 0;
	}

	if (strcmp(op, "fdisk") == 0 || strcmp(op, "mkfs") == 0) {
		if (argc < 4 || parse_volume(argv[2], &volume) != 0) {
			usage(csh);
			return -1;
		}
		if (strcmp(argv[3], "yes") != 0) {
			csh_printf(csh, "stor: %s destroys the partition table /"
					" filesystem on %s; re-run with 'yes'\r\n",
				   op, blkdev_name(volume));
			return -1;
		}
		if (op[0] == 'f') {
			cmd_fdisk(csh, volume);
		} else {
			cmd_mkfs(csh, volume);
		}
		return 0;
	}

	usage(csh);
	return -1;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_stor, stor,
			  "stor [mount|umount|ls|rd|cat|wr|find|fdisk|mkfs] ...",
			  "block devices and FAT volumes over them");