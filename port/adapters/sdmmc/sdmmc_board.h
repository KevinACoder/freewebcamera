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

#endif /* SDMMC_BOARD_H */
