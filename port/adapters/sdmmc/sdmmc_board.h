/*
 * @file   sdmmc_board.h
 * @brief  The SDMMC adapter's own declarations: the board clock facts
 *         (fparameters.h equivalents of the standalone line, DESIGN §10) and
 *         the memory API the adapter adds on top of the vendored osa layer.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef SDMMC_BOARD_H
#define SDMMC_BOARD_H

#include "fsl_sdmmc_common.h"	/* status_t */
#include "fsl_sdmmc_host.h"	/* sdmmchost_t */

#define DWMMC_CIU_CLOCK_HZ	150000000U /* dw-mmc ciu max-frequency */
#define DWMSHC_CCLK_CLOCK_HZ	200000000U /* CCLK_EMMC source */

/*
 * Release a buffer from SDMMC_OSAMemoryAlignedAllocate. The vendored header
 * only declares the plain SDMMC_OSAMemoryFree, which is not safe here: our
 * aligned allocator keeps the raw heap pointer in the word in front of the
 * payload, so plain Free would hand the heap an interior pointer. Kept in
 * this adapter header because the vendored tree stays byte-identical.
 */
void SDMMC_OSAMemoryAlignedFree(void *p);

/*
 * SDIO card interrupt (DAT1 -> RINTSTS bit 16) for the dw-mmc host, the
 * M11 r4 interrupt-mode channel. establish installs the controller IRQ
 * (also on the poll line, which otherwise never unmasks the GIC input),
 * registers the upcall and arms the SDIO_INT line; the upcall runs in ISR
 * context and must only wake a worker. The ISR self-masks the bit, so a
 * level-held DAT1 cannot storm the GIC; the consumer calls ack() after it
 * has consumed the chip-side source (HISR written) to re-arm the line.
 * release disarms everything (stop/teardown path).
 */
status_t dwmmc_host_sdio_int_establish(sdmmchost_t *host,
    void (*upcall)(void *), void *arg);
void dwmmc_host_sdio_int_ack(sdmmchost_t *host);
void dwmmc_host_sdio_int_release(sdmmchost_t *host);

#endif /* SDMMC_BOARD_H */
