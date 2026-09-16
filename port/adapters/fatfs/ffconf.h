/*-----------------------------------------------------------------------*/
/*  FatFs R0.16 configuration for this project - the shadow config header */
/*-----------------------------------------------------------------------*/
/*
 * FatFs reaches its configuration from ff.c/ff.h as `#include "ffconf.h"`,
 * which resolves relative to the including file first: that is why the
 * vendored tree (third-party/fatfs/) deliberately does NOT contain upstream's
 * ffconf.h - the one in this adapter directory is the only one that exists,
 * and it is the whole of this integration's file-system feature set.
 *
 * The same arrangement the lwIP adapter uses with lwipopts.h and the shell
 * adapter with csh_config.h. Nothing here modifies upstream code.
 *
 * FFCONF_DEF must match FF_DEFINED in the vendored ff.h: FatFs refuses to
 * compile against a mismatched configuration revision.
 *
 * Values that were decided rather than copied from upstream's template are
 * commented with the reason; everything else is upstream's default.
 */

#define FFCONF_DEF	80386	/* Revision ID of this configuration */

/*--- Function Configurations -------------------------------------------*/

#define FF_FS_READONLY	0	/* 0: read/write. Writes are what the M3
				 * acceptance exercises, and the disks are
				 * the lab's own test media. */

#define FF_FS_MINIMIZE	0	/* 0: all basic functions */

#define FF_USE_FIND	1	/* f_findfirst/f_findnext: used by the
				 * `stor find` command, and cheap */

#define FF_USE_MKFS	1	/* f_mkfs/f_fdisk: the three devices have no
				 * FAT volume until one is made */

#define FF_USE_FASTSEEK	0	/* 0: no file seek acceleration table */

#define FF_USE_EXPAND	0	/* 0: no f_expand */

#define FF_USE_CHMOD	1	/* f_chmod/f_utime: harmless, and a file
				 * command set that cannot even clear the
				 * read-only bit is a nuisance */

#define FF_USE_LABEL	0	/* 0: no volume label API */

#define FF_USE_FORWARD	0	/* 0: no f_forward streaming */

#define FF_USE_STRFUNC	1	/* 1: f_puts/f_gets/f_printf/f_gets are
				 * used by the `stor cat`/`stor wr`
				 * commands */

#define FF_PRINT_LLI	1	/* 1: `%llu` in f_printf (passed 64-bit
				 * sizes need it) */
#define FF_PRINT_FLOAT	0	/* 0: no floating point in f_printf */
#define FF_STRF_ENCODE	0	/* 0: ANSI/OEM in the stream functions */

/*--- Locale and Namespace Configurations --------------------------------*/

/* Code page 437 (US): the FAT volume here holds the project's own files with
 * ASCII names, and 437 keeps ffunicode.c's tables small. */
#define FF_CODE_PAGE	437

/* LFN on, with the dynamic buffer (mode 2): names like "index.html" fit better
 * in 255 characters than in 8.3, and mode 2 is the one that pairs with the
 * thread-safe configuration below - FatFs rejects mode 1 outright when
 * FF_FS_REENTRANT is on, because a static name buffer would be shared between
 * threads operating on different volumes. The buffer comes from ff_memalloc
 * (fatfs_os.c, the kernel heap), one per mounted volume. */
#define FF_USE_LFN	2
#define FF_MAX_LFN	255
#define FF_LFN_UNICODE	0	/* 0: ANSI/OEM in the API (ASCII names) */
#define FF_LFN_BUF	255
#define FF_SFN_BUF	12

#define FF_FS_RPATH	0	/* 0: no relative paths; commands pass
				 * volume-qualified paths ("0:/dir/file") */
#define FF_PATH_DEPTH	10

/* Three logical volumes, one per block device (see include/fs.h). */
#define FF_VOLUMES	3
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"0","1","2"

/* One physical drive per volume, and each volume is the drive's partition 1.
 * This is what makes f_fdisk/f_mkfs addressable per volume and what makes a
 * GPT disk require re-partitioning before it can be mounted: FatFs reads MBR
 * partition tables, not GPT. */
#define FF_MULTI_PARTITION	1

#define FF_MIN_SS	512
#define FF_MAX_SS	512
#define FF_LBA64	0	/* 32-bit sector addressing: 238 GB is far
				 * from the 2 TiB line and this keeps LBA_t a
				 * natural-width type */
#define FF_MIN_GPT	0x10000000
#define FF_USE_TRIM	0

/*--- System Configurations ---------------------------------------------*/

#define FF_FS_TINY	0	/* 0: each volume keeps its own window
				 * buffer (no per-API shared window) */

#define FF_FS_EXFAT	0	/* 0: no exFAT */

/* No RTC has been ported yet (M3 keeps to storage; the RK809 RTC is an I2C
 * driver away). FatFs then stamps files with one fixed date instead of
 * calling get_fattime(). */
#define FF_FS_NORTC	1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026

#define FF_FS_CRTIME	0	/* 0: no creation-time field handling */
#define FF_FS_NOFSINFO	0	/* 0: use the free-cluster hints in FSINFO */
#define FF_FS_LOCK	0	/* 0: no file lock table (single writer) */

/* Volume mutexes, implemented on CMSIS-RTOS2 in fatfs_os.c. Without them a
 * shell command, the network thread and a future httpd could interleave
 * f_read/f_write on one volume and corrupt the window buffer - the failure
 * looks like random data, not like a race. */
#define FF_FS_REENTRANT	1
#define FF_FS_TIMEOUT	1000	/* ticks; the CMSIS mutex timeout is in ms */