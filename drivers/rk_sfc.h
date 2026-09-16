/*
 * @file   rk_sfc.h
 * @brief  RK3568 SFC / SPI-NOR (read-only) - CMSIS-Driver instance + probe API.
 *
 * The on-board W25Q64DW (8 MiB) holds the bootloader; this driver is READ
 * ONLY by construction:
 *   - the internal exec path has no write direction at all;
 *   - only four opcodes can ever reach the wire (JEDEC 0x9F, SFDP 0x5A,
 *     RDSR 0x05, READ 0x03);
 *   - the ARM_DRIVER_FLASH facade returns UNSUPPORTED for ProgramData,
 *     EraseSector and EraseChip;
 *   - no shell command exposes a write operation.
 *
 * Consumers:
 *   Driver_Flash0            standard ARM_DRIVER_FLASH (ReadData only)
 *   rk_sfc_get_jedec(id)     the 3-byte JEDEC ID captured at init (EF 60 17)
 *   rk_sfc_read_sfdp(a,b,l)  SFDP read through the same whitelist
 *
 * @author zhugengyu
 * @date   17.09.2026
 */

#ifndef FREEWEBCAMERA_RK_SFC_H
#define FREEWEBCAMERA_RK_SFC_H

#include <stdint.h>
#include "Driver_Flash.h"

extern ARM_DRIVER_FLASH Driver_Flash0;

/* JEDEC ID (manufacturer, type, capacity) read during initialization.
 * Returns 0 on success. */
int rk_sfc_get_jedec(uint8_t id[3]);

/* SFDP basic read (3-byte address, 8 dummy cycles). */
int rk_sfc_read_sfdp(uint32_t addr, void *buf, uint32_t len);

#endif /* FREEWEBCAMERA_RK_SFC_H */
