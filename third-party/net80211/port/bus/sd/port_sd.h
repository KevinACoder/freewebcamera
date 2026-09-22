/*
 * @file
 * @brief SDIO bus section of the port interface.
 *
 * Mirrors port/bus/usb/port_usb.h: a bus backend claims the wlan I/O
 * function of an enumerated SDIO card and hands it to the matched chip
 * driver as a wlan_sdio_dev. The register file goes through CMD52
 * (one-byte, also the only reliable form before the MAC is powered)
 * and CMD53 incremental transfers (2/4-byte registers and the FIFO
 * bursts); the ops are per-device so the backend binding stays in the
 * claiming environment (fsl_sdio today).
 *
 * @date 22.09.2026
 * @author zhugengyu
 */

#ifndef NET80211_PORT_SD_H_
#define NET80211_PORT_SD_H_

#include <stdint.h>

struct wlan_sdio_bus_ops;

/* A claimed SDIO wlan function. The port adapter fills this from its
 * card handle; the NetBSD-shim world only sees opaque pointers. */
struct wlan_sdio_dev {
	void *port_priv; /* port-owned object (sdmmc_function shell) */
	void *env_card;  /* backend card handle (fsl sdio_card_t) */
	const struct wlan_sdio_bus_ops *ops;

	uint16_t vendor;  /* CIS manufacturer (function 0) */
	uint16_t product; /* CIS product (function 0) */
	uint8_t function; /* the wlan I/O function number (1) */
};

struct wlan_sdio_bus_ops {
	int (*set_blocklen)(struct wlan_sdio_dev *dev, int len);
	int (*func_enable)(struct wlan_sdio_dev *dev);

	uint8_t (*read_1)(struct wlan_sdio_dev *dev, uint32_t addr);
	uint16_t (*read_2)(struct wlan_sdio_dev *dev, uint32_t addr);
	uint32_t (*read_4)(struct wlan_sdio_dev *dev, uint32_t addr);
	int (*write_1)(struct wlan_sdio_dev *dev, uint32_t addr,
	    uint8_t val);
	int (*write_2)(struct wlan_sdio_dev *dev, uint32_t addr,
	    uint16_t val);
	int (*write_4)(struct wlan_sdio_dev *dev, uint32_t addr,
	    uint32_t val);

	/* CMD53 bursts; len > 512 goes out in block mode with 512-byte
	 * blocks (set_blocklen must have run), len <= 512 in byte mode.
	 * Both incremental (op code = 1). */
	int (*read_region)(struct wlan_sdio_dev *dev, uint32_t addr,
	    uint8_t *buf, int len);
	int (*write_region)(struct wlan_sdio_dev *dev, uint32_t addr,
	    const uint8_t *buf, int len);
};

/* SDIO match table entry (CIS manufacturer/product of function 0),
 * terminated by vendor==0. */
struct wlan_sdio_id {
	uint16_t vendor;
	uint16_t product;
};

#endif /* NET80211_PORT_SD_H_ */
