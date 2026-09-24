/*
 * @file   nvme_diag.c
 * @brief  NVMe bring-up and read diagnostics, callable from any shell.
 *
 * Lives in app/ (D48): a test/diagnostic is application code. It reaches
 * the driver only through the frozen include/nvme.h interface.
 *
 * The driver (dwc_nvme.c) reaches the disk through PCIe + MSI-X, and the
 * first completed I/O command makes it print its message-interrupt
 * delivery evidence from task context - which is the end-to-end ITS LPI
 * proof, and exactly what the ThreadX mainline image had no path to
 * produce: the driver compiles there, but the FatFs binding that probes
 * it on FreeRTOS is stubbed out. This module is that missing probe,
 * kernel-agnostic on purpose:
 *
 *   nvme          bring up (idempotent), report every controller:
 *                 model, geometry, queue depth
 *   nvme read N   read 8 blocks at LBA N (default 0), print leading
 *                 quadwords and a checksum so repeated runs against the
 *                 same LBA are comparable by eye
 *
 * Read-only by policy: the acceptance question is interrupt delivery,
 * and nothing here may disturb a filesystem another image mounts on the
 * same disk. Writes stay with the FatFs path (board-proven M3-A).
 *
 * @author zhugengyu
 * @date   21.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "board.h"
#include "nvme.h"

/* The instance lives in dwc_nvme.c; consumers declare it locally, same as
 * the FatFs block binding. */
extern ARM_DRIVER_NVME Driver_NVME;

/* Eight blocks: one PRP pair at most, and enough bytes that a stuck bus
 * shows up as the all-0xff / all-0x00 patterns it actually produces. */
#define NVME_DIAG_BLOCKS	8U
#define NVME_DIAG_LBA_SIZE	512U

/* The read lands here; 64-byte alignment keeps the driver's dcache
 * maintenance honest without betting on the line size. */
static uint64_t diag_blocks[NVME_DIAG_BLOCKS * NVME_DIAG_LBA_SIZE / 8U]
	__attribute__((aligned(64)));

/* "0x10" / "16"; -1 on garbage (same shape as itsdump's parser). */
static int diag_parse_u32(const char *s)
{
	uint32_t value = 0U;
	unsigned int base = 10U;

	if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
		base = 16U;
		s += 2;
	}
	if (*s == '\0') {
		return -1;
	}
	for (; *s != '\0'; s++) {
		uint32_t digit;

		if ((*s >= '0') && (*s <= '9')) {
			digit = (uint32_t)(*s - '0');
		} else if ((base == 16U) && (*s >= 'a') && (*s <= 'f')) {
			digit = (uint32_t)(*s - 'a') + 10U;
		} else if ((base == 16U) && (*s >= 'A') && (*s <= 'F')) {
			digit = (uint32_t)(*s - 'A') + 10U;
		} else {
			return -1;
		}
		value = (value * base) + digit;
	}
	return (int)value;
}

static void diag_report(uint32_t ctrl)
{
	NVME_CAPABILITIES caps = Driver_NVME.GetCapabilities(ctrl);
	char model[48];

	if (Driver_NVME.Identify(ctrl, model, sizeof(model)) ==
	    ARM_DRIVER_OK) {
		board_log("nvme-diag: ctrl %u model \"%s\"\n", ctrl, model);
	} else {
		board_log("nvme-diag: ctrl %u identify failed\n", ctrl);
	}
	board_log("nvme-diag: ctrl %u %llu blocks x %u bytes, qd %u, "
		  "max_transfer %u, timeout %u ms\n",
		  ctrl, (unsigned long long)caps.sector_count,
		  caps.sector_size, caps.queue_depth, caps.max_transfer,
		  caps.timeout_ms);
}

static int diag_read(int lba)
{
	const uint32_t ctrl = 0U;
	uint64_t sum = 0;
	uint32_t done;
	uint32_t i;
	int32_t ret;

	ret = Driver_NVME.ReadBlocks(ctrl, (uint64_t)lba, diag_blocks,
				     NVME_DIAG_BLOCKS);
	if (ret != ARM_DRIVER_OK) {
		board_log("nvme-diag: read lba %d x %u failed: %d\n", lba,
			  NVME_DIAG_BLOCKS, ret);
		return -1;
	}

	for (i = 0; i < sizeof(diag_blocks) / sizeof(diag_blocks[0]); i++) {
		sum += diag_blocks[i];
	}
	board_log("nvme-diag: read lba %d ok: %016llx %016llx sum %016llx\n",
		  lba, (unsigned long long)diag_blocks[0],
		  (unsigned long long)diag_blocks[1],
		  (unsigned long long)sum);

	done = Driver_NVME.GetTransferStatus(ctrl);
	board_log("nvme-diag: %u completions since last query\n", done);

	return 0;
}

int nvme_diag_cmd(int argc, char **argv)
{
	(void)argc;

	/* Bring-up goes through the frozen interface: Initialize() is
	 * idempotent and brings the PCIe backend up itself. */
	if (Driver_NVME.Initialize(0U, NULL) != ARM_DRIVER_OK) {
		board_log("nvme-diag: no NVMe controller came up\n");
		return -1;
	}
	diag_report(0U);

	if ((argv != NULL) && (argv[1] != NULL) &&
	    (strcmp(argv[1], "read") == 0)) {
		int lba = 0;

		if (argv[2] != NULL) {
			lba = diag_parse_u32(argv[2]);
			if (lba < 0) {
				board_log("nvme-diag: bad lba '%s'\n", argv[2]);
				return -1;
			}
		}
		return diag_read(lba);
	}

	return 0;
}
