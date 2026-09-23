/*
 * @file
 * @brief sdmmc(4) shell for the imported SDIO wlan drivers.
 *
 * Only the slice rtw8189f(4) touches: the function/softc shells the
 * autoconf glue walks (match reads the function-0 CIS), and the
 * sdmmc_io_* register file, which the SDIO bus backend implements
 * through its per-device wlan_sdio_bus_ops (CMD52 for one-byte, CMD53
 * incremental for everything else - same shapes as NetBSD).
 */

#ifndef _COMPAT_DEV_SDMMC_SDMMCVAR_H_
#define _COMPAT_DEV_SDMMC_SDMMCVAR_H_

#include <sys/cdefs.h>
#include <sys/types.h>

#include <port/bus/sd/port_sd.h>

struct sdmmc_cis {
	uint16_t manufacturer;
	uint16_t product;
};

struct sdmmc_softc;

struct sdmmc_function {
	struct sdmmc_softc *sc; /* parent (function 0 owner) */
	int number;		/* function number, 0 = common */

	/* function 0 carries the common CIS */
	struct sdmmc_cis cis;

	/* the claimed wlan function behind this shell */
	struct wlan_sdio_dev *sf_port;
};

struct sdmmc_softc {
	struct sdmmc_function *sc_fn0; /* function 0 */
};

struct sdmmc_attach_args {
	struct sdmmc_function *sf;
};

uint8_t sdmmc_io_read_1(struct sdmmc_function *, uint32_t);
uint16_t sdmmc_io_read_2(struct sdmmc_function *, uint32_t);
uint32_t sdmmc_io_read_4(struct sdmmc_function *, uint32_t);
int sdmmc_io_write_1(struct sdmmc_function *, uint32_t, uint8_t);
int sdmmc_io_write_2(struct sdmmc_function *, uint32_t, uint16_t);
int sdmmc_io_write_4(struct sdmmc_function *, uint32_t, uint32_t);
int sdmmc_io_read_region_1(struct sdmmc_function *, uint32_t,
    u_char *, int);
int sdmmc_io_write_region_1(struct sdmmc_function *, uint32_t,
    const u_char *, int);
int sdmmc_io_set_blocklen(struct sdmmc_function *, int);
int sdmmc_io_function_enable(struct sdmmc_function *);

/* NET80211_PORT(L): SDIO card interrupt (M11 r4). NetBSD establishes a
 * handler on the host controller and re-arms internally; our line is
 * level-signalled and the ISR self-masks it (a DAT1 held would storm
 * the GIC), so the consumer acks after draining. The cookie feeds ack
 * and disestablish (stop path). */
void *sdmmc_intr_establish(struct sdmmc_function *, int (*)(void *), void *);
void sdmmc_intr_ack(void *);
void sdmmc_intr_disestablish(void *);

#endif /* _COMPAT_DEV_SDMMC_SDMMCVAR_H_ */
