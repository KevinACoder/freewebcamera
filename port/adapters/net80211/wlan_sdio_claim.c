/*
 * @file   wlan_sdio_claim.c
 * @brief  SDIO bus backend for the net80211 adapter: the fsl_sdio ops
 *         the rtw8189f chip driver drives through the sdmmc(9) shim,
 *         plus the probe that claims a matched card off the sdmmc0
 *         slot (the RTL8189FTV).
 *
 * Claim model (the SDIO counterpart of usbh_urtwn_class.c): the slot
 * enumerates inside sdio_start() - there is no hotplug hook in the
 * fsl_sdmmc flow - so the claim runs as an explicit probe, called by
 * the app once the wlan services are up (task_usb_start, after
 * wlan_start()) and again after sdio_start() succeeds, whichever runs
 * last wins; the probe is idempotent.
 *
 * DMA staging: dw_mmc flushes/invalidate the transfer buffer with the
 * range rounded OUTWARD to full cache lines, so an RX buffer that
 * shares a line with live data gets its neighbors discarded. All
 * data bursts therefore go through this file's own 512-byte-aligned
 * staging buffer, which owns whole lines (and doubles as the bounce
 * buffer alignment the CMD53 FIFO protocol wants).
 *
 * @author zhugengyu
 * @date   22.09.2026
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "fsl_sdio.h"

#include <port/port.h>
#include <port/bus/sd/port_sd.h>
#include <port/osal/wlan_port_core.h>

/* the card handle the sdmmc adapter owns (enumerated by sdio_start) */
extern void *sdio_card_get(void);
/* wlan_start() sets this once the OSAL services + firmware registry exist */
extern int wlan_adapter_ready(void);
/* the compiled-in chip drivers (wlan_adapter.c) */
extern const struct wlan_chip_driver *const wlan_chip_drivers[];

/* covers the driver's largest FIFO burst (RTW8189F_RXBUFSZ 8 KiB);
 * 512-aligned so every CMD53 byte-mode count and every 512-byte block
 * transfer DMAs straight from here (the fsl layer only reroutes to its
 * own aligned buffer when the address misses blockSize alignment) */
#define WLAN_SDIO_STAGE_SZ	(8192u + 512u)
static uint8_t wlan_sdio_stage[WLAN_SDIO_STAGE_SZ]
    __attribute__((aligned(512)));

/* ------------------------------------------------------------------ */
/* the fsl_sdio binding of the bus ops */

static int sd_fsl_check(status_t st) {
	return (st == kStatus_Success) ? 0 : -1;
}

static int sd_fsl_set_blocklen(struct wlan_sdio_dev *dev, int len) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;

	return sd_fsl_check(SDIO_SetBlockSize(card,
	    (sdio_func_num_t) dev->function, (uint32_t) len));
}

static int sd_fsl_func_enable(struct wlan_sdio_dev *dev) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;

	return sd_fsl_check(SDIO_EnableIO(card,
	    (sdio_func_num_t) dev->function, true));
}

static uint8_t sd_fsl_read_1(struct wlan_sdio_dev *dev, uint32_t addr) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;
	uint8_t val = 0xffu;

	(void) SDIO_IO_Read_Direct(card, (sdio_func_num_t) dev->function,
	    addr, &val);
	return val;
}

static int sd_fsl_write_1(struct wlan_sdio_dev *dev, uint32_t addr,
    uint8_t val) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;

	return sd_fsl_check(SDIO_IO_Write_Direct(card,
	    (sdio_func_num_t) dev->function, addr, &val, false));
}

/* 2/4-byte accesses go out as CMD53 incremental transfers, padded to
 * 4 bytes on the wire: the dw_mmc host rejects any data length that is
 * not a 4-byte multiple (IDMAC constraint), so a bare 2-byte read is
 * declined host-side. Over-reading two neighbouring registers on a
 * read is harmless; a write must not touch them, so 2-byte writes go
 * out as two CMD52s instead. */
static uint32_t sd_fsl_read_bytes(struct wlan_sdio_dev *dev,
    uint32_t addr, unsigned n) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;
	uint32_t val = 0;

	(void) SDIO_IO_Read_Extended(card, (sdio_func_num_t) dev->function,
	    addr, wlan_sdio_stage, 4u, (uint32_t) SDIO_EXTEND_CMD_OP_CODE_MASK);
	memcpy(&val, wlan_sdio_stage, n);
	return val;
}

static int sd_fsl_write_bytes(struct wlan_sdio_dev *dev, uint32_t addr,
    uint32_t val, unsigned n) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;
	uint8_t b;
	unsigned i;
	int err = 0;

	if (n < 4u) {
		/* sub-dword write: CMD52 per byte, no neighbour clobber */
		for (i = 0; i < n && err == 0; i++) {
			b = (uint8_t) (val >> (8u * i));
			err = sd_fsl_write_1(dev, addr + i, b);
		}
		return err;
	}

	memcpy(wlan_sdio_stage, &val, n);
	return sd_fsl_check(SDIO_IO_Write_Extended(card,
	    (sdio_func_num_t) dev->function, addr, wlan_sdio_stage, n,
	    (uint32_t) SDIO_EXTEND_CMD_OP_CODE_MASK));
}

static uint16_t sd_fsl_read_2(struct wlan_sdio_dev *dev, uint32_t addr) {
	return (uint16_t) sd_fsl_read_bytes(dev, addr, 2u);
}

static uint32_t sd_fsl_read_4(struct wlan_sdio_dev *dev, uint32_t addr) {
	return sd_fsl_read_bytes(dev, addr, 4u);
}

static int sd_fsl_write_2(struct wlan_sdio_dev *dev, uint32_t addr,
    uint16_t val) {
	return sd_fsl_write_bytes(dev, addr, val, 2u);
}

static int sd_fsl_write_4(struct wlan_sdio_dev *dev, uint32_t addr,
    uint32_t val) {
	return sd_fsl_write_bytes(dev, addr, val, 4u);
}

/* CMD53: <= 512 bytes in byte mode (count = bytes), the rest in
 * 512-byte block mode where the fsl layer takes count = BLOCK COUNT
 * and reads the block size back from the FBR (the driver sets 512 via
 * set_blocklen at attach). The wire length the driver hands over is
 * rounded to 4/512 already, so the block split is exact */
static int sd_fsl_read_region(struct wlan_sdio_dev *dev, uint32_t addr,
    uint8_t *buf, int len) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;
	uint32_t flags = (uint32_t) SDIO_EXTEND_CMD_OP_CODE_MASK;
	uint32_t count = (uint32_t) len;
	status_t st;

	if (len > 512) {
		flags |= (uint32_t) SDIO_EXTEND_CMD_BLOCK_MODE_MASK;
		count = (uint32_t) len / 512u;
	}
	st = SDIO_IO_Read_Extended(card, (sdio_func_num_t) dev->function,
	    addr, wlan_sdio_stage, count, flags);
	if (st == kStatus_Success) {
		memcpy(buf, wlan_sdio_stage, (size_t) len);
		return 0;
	}
	return -1;
}

static int sd_fsl_write_region(struct wlan_sdio_dev *dev, uint32_t addr,
    const uint8_t *buf, int len) {
	sdio_card_t *card = (sdio_card_t *) dev->env_card;
	uint32_t flags = (uint32_t) SDIO_EXTEND_CMD_OP_CODE_MASK;
	uint32_t count = (uint32_t) len;

	if (len > (int) sizeof(wlan_sdio_stage)) {
		return -1;
	}
	memcpy(wlan_sdio_stage, buf, (size_t) len);
	if (len > 512) {
		flags |= (uint32_t) SDIO_EXTEND_CMD_BLOCK_MODE_MASK;
		count = (uint32_t) len / 512u;
	}
	return sd_fsl_check(SDIO_IO_Write_Extended(card,
	    (sdio_func_num_t) dev->function, addr, wlan_sdio_stage, count,
	    flags));
}

static const struct wlan_sdio_bus_ops sd_fsl_ops = {
	.set_blocklen = sd_fsl_set_blocklen,
	.func_enable = sd_fsl_func_enable,
	.read_1 = sd_fsl_read_1,
	.read_2 = sd_fsl_read_2,
	.read_4 = sd_fsl_read_4,
	.write_1 = sd_fsl_write_1,
	.write_2 = sd_fsl_write_2,
	.write_4 = sd_fsl_write_4,
	.read_region = sd_fsl_read_region,
	.write_region = sd_fsl_write_region,
};

/* ------------------------------------------------------------------ */
/* the probe: match the registry against the enumerated card */

static const struct wlan_chip_driver *wlan_sdio_id_match(uint16_t vendor,
	uint16_t product) {
	const struct wlan_chip_driver *drv;
	const struct wlan_sdio_id *id;
	int i;

	for (i = 0; wlan_chip_drivers[i] != NULL; i++) {
		drv = wlan_chip_drivers[i];
		if (drv->bus != WLAN_BUS_SDIO) {
			continue;
		}
		for (id = drv->sdio_ids;
		    id->vendor != 0 || id->product != 0; id++) {
			if (id->vendor == vendor && id->product == product) {
				return drv;
			}
		}
	}
	return NULL;
}

int wlan_sdio_probe(void) {
	static int claimed;
	sdio_card_t *card;
	const struct wlan_chip_driver *drv;
	struct wlan_port_iface *pif;
	struct wlan_sdio_dev *dev;
	uint16_t vendor, product;

	if (claimed) {
		return 0;
	}
	if (!wlan_adapter_ready()) {
		return -1;
	}
	card = (sdio_card_t *) sdio_card_get();
	if (card == NULL) {
		return -1;
	}

	vendor = (uint16_t) card->commonCIS.mID;
	product = (uint16_t) card->commonCIS.mInfo;
	drv = wlan_sdio_id_match(vendor, product);
	if (drv == NULL) {
		return -1;
	}
	if (wlan_port_if_n >= WLAN_PORT_MAX_IF) {
		printf("wlan: another adapter already claimed\n");
		return -1;
	}

	pif = malloc(sizeof(*pif));
	if (pif == NULL) {
		return -1;
	}
	memset(pif, 0, sizeof(*pif));
	pif->drv = drv;

	dev = &pif->sdio;
	dev->env_card = card;
	dev->ops = &sd_fsl_ops;
	dev->vendor = vendor;
	dev->product = product;
	dev->function = 1; /* the wlan I/O function */

	wlan_port_ifs[wlan_port_if_n++] = pif;
	claimed = 1;

	printf("wlan: %s found on sdio fn%u (%04x:%04x)\n",
	    drv->name, dev->function, vendor, product);

	if (drv->attach(dev, NULL) == 0) {
		pif->attached = 1;
		return 0;
	}
	printf("wlan: %s attach failed\n", drv->name);
	return -1;
}
