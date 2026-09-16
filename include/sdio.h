/*
 * @file   sdio.h
 * @brief  SDIO card enumeration on the sdmmc0 slot - the interface the app
 *         layer sees.
 *
 * The slot carries the RTL8189FTV WiFi module; this milestone enumerates the
 * card (CMD5/CMD3/CMD7, then the CCCR/FBR/CIS walk over CMD52) and reports
 * the result. The wireless function itself is a later milestone - nothing
 * here transmits.
 *
 * Same seam shape as fs.h and net.h: the app calls sdio_start() once from a
 * task, and the implementation behind it lives in an adapter.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_SDIO_H
#define FREEWEBCAMERA_SDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enumerate the SDIO card. Idempotent: a second call is a no-op returning
 * the first result. 0 = card enumerated; nonzero = no card or bus failure. */
int sdio_start(void);

/* True once an enumeration has succeeded (the card descriptor is live). */
bool sdio_card_up(void);

/* Re-run the enumeration from scratch (deinit + init). Shell-facing. */
int sdio_restart(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_SDIO_H */
