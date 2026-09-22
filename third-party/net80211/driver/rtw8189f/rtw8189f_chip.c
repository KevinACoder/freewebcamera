/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Chip-level routines for the RTL8189FTV: power-on sequence, the
 * power-on self test, eFuse access and firmware download.
 *
 * Sequences are transcribed from the vendor rtl8189fs driver
 * (hal/rtl8188f): the card-enable power flow lives in Hal8188FPwrSeq.h
 * (CARDDIS_TO_CARDEMU + CARDEMU_TO_ACT), the self test in
 * hal/hal_hci/hal_sdio.c sdio_power_on_check(), and the firmware
 * download in rtl8188f_hal_init.c rtl8188f_FirmwareDownload().  The
 * board's Linux ground truth shows the self test reporting
 * val_mix:0x0000063f / 0x1B8 test Pass, which is what these sequences
 * must reproduce.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>
#include <sys/endian.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <dev/sdmmc/sdmmcvar.h>

#include "rtw8189fvar.h"
#include "rtw8189f_tables.h"

#ifdef RTW8189F_DEBUG
/* 0x30 = INIT|TX: newstate transitions, per-mgmt TX, non-beacon mgmt RX --
 * association-attempt volumes only.  Never leave non-zero: sustained scan
 * floods kill console input within minutes. */
int rtw8189f_debug = 0;
#endif

static void	rtw8189f_txpwr_parse(struct rtw8189f_softc *);
static void	rtw8189f_set_txpower(struct rtw8189f_softc *, unsigned);
static void	rtw8189f_iqk(struct rtw8189f_softc *);

/* ------------------------------------------------------------------ */
/* Power sequence                                                      */
/* ------------------------------------------------------------------ */

#define RTW8189F_PWR_BASE_MAC		0	/* WLAN I/O register */
#define RTW8189F_PWR_BASE_SDIO		1	/* SDIO local register */

#define RTW8189F_PWR_CMD_WRITE		1
#define RTW8189F_PWR_CMD_POLLING	2
#define RTW8189F_PWR_CMD_DELAY		3
#define RTW8189F_PWR_CMD_END		0

struct rtw8189f_pwr_cmd {
	uint32_t	addr;
	uint8_t		base;
	uint8_t		cmd;
	uint8_t		mask;
	uint8_t		value;
};

/*
 * rtl8188F_card_enable_flow = TRANS_CARDDIS_TO_CARDEMU +
 * TRANS_CARDEMU_TO_ACT (include/Hal8188FPwrSeq.h), SDIO-relevant steps
 * only.
 */
static const struct rtw8189f_pwr_cmd rtw8189f_card_enable_flow[] = {
	/* leave suspend: SDIO-local 0x86 bit0 = 0, poll bit1 -> 1 */
	{ 0x0086, RTW8189F_PWR_BASE_SDIO, RTW8189F_PWR_CMD_WRITE,
	  __BIT(0), 0 },
	{ 0x0086, RTW8189F_PWR_BASE_SDIO, RTW8189F_PWR_CMD_POLLING,
	  __BIT(1), __BIT(1) },
	/* 0x04[12:11] = 0: disable WL suspend */
	{ 0x0005, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_WRITE,
	  __BIT(3) | __BIT(4), 0 },
	/* 0x04[10] = 0: disable SW LPS */
	{ 0x0005, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_WRITE,
	  __BIT(2), 0 },
	/* wait until 0x04[17] = 1: power ready */
	{ 0x0006, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_POLLING,
	  __BIT(1), __BIT(1) },
	/* 0x04[15] = 0: disable HW power down */
	{ 0x0005, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_WRITE,
	  __BIT(7), 0 },
	/* 0x04[11] = 0: disable WL suspend (again, per vendor flow) */
	{ 0x0005, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_WRITE,
	  __BIT(3), 0 },
	/* 0x04[8] = 1, then poll until 0: switch the power state machine */
	{ 0x0005, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_WRITE,
	  __BIT(0), __BIT(0) },
	{ 0x0005, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_POLLING,
	  __BIT(0), 0 },
	/* 0x27 <= 0x35: xtal_qsel = 1, reduces RF noise */
	{ 0x0027, RTW8189F_PWR_BASE_MAC, RTW8189F_PWR_CMD_WRITE,
	  0xff, 0x35 },
	{ 0, 0, RTW8189F_PWR_CMD_END, 0, 0 },
};

static int
rtw8189f_pwr_seq_step(struct rtw8189f_softc *sc,
    const struct rtw8189f_pwr_cmd *cmd)
{
	uint8_t cur;
	int retry;

	switch (cmd->base) {
	case RTW8189F_PWR_BASE_SDIO:
		switch (cmd->cmd) {
		case RTW8189F_PWR_CMD_WRITE:
			cur = rtw8189f_sdiolocal_read_1(sc, cmd->addr);
			rtw8189f_sdiolocal_write_1(sc, cmd->addr,
			    (cur & ~cmd->mask) | (cmd->value & cmd->mask));
			return 0;
		case RTW8189F_PWR_CMD_POLLING:
			for (retry = 0; retry < 100; retry++) {
				cur = rtw8189f_sdiolocal_read_1(sc, cmd->addr);
				if ((cur & cmd->mask) == (cmd->value & cmd->mask))
					return 0;
				kpause("rtw8189fpw", false, 1, NULL);
			}
			return EIO;
		}
		break;

	default: /* RTW8189F_PWR_BASE_MAC */
		switch (cmd->cmd) {
		case RTW8189F_PWR_CMD_WRITE:
			cur = rtw8189f_mac_read_1(sc, cmd->addr);
			rtw8189f_mac_write_1(sc, cmd->addr,
			    (cur & ~cmd->mask) | (cmd->value & cmd->mask));
			return 0;
		case RTW8189F_PWR_CMD_POLLING:
			for (retry = 0; retry < 100; retry++) {
				cur = rtw8189f_mac_read_1(sc, cmd->addr);
				if ((cur & cmd->mask) == (cmd->value & cmd->mask))
					return 0;
				kpause("rtw8189fpw", false, 1, NULL);
			}
			return EIO;
		}
		break;
	}

	return EINVAL;
}

static int
rtw8189f_pwr_seq(struct rtw8189f_softc *sc,
    const struct rtw8189f_pwr_cmd *flow)
{
	int error;

	for (; flow->cmd != RTW8189F_PWR_CMD_END; flow++) {
		error = rtw8189f_pwr_seq_step(sc, flow);
		if (error != 0) {
			aprint_error_dev(sc->sc_dev,
			    "power sequence failed at 0x%04x step %d (%d)\n",
			    flow->addr, flow->cmd, error);
			return error;
		}
	}

	return 0;
}

int
rtw8189f_power_on(struct rtw8189f_softc *sc)
{
	uint16_t cr;

	/* Unlock the ISO/CLK/power control registers. */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_RSV_CTRL, 0x00);

	DNPRINTF(sc, RTW8189F_DBG_PWR, "running card enable flow\n");
	if (rtw8189f_pwr_seq(sc, rtw8189f_card_enable_flow) != 0)
		return EIO;

	/* Enable MAC DMA/WMAC/SCHEDULE/SEC blocks. */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_CR, 0x00);
	cr = rtw8189f_mac_read_2(sc, RTW8189F_REG_CR);
	cr |= RTW8189F_CR_POWERON;
	rtw8189f_mac_write_2(sc, RTW8189F_REG_CR, cr);

	sc->sc_mac_on = true;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Power-on self test                                                  */
/* ------------------------------------------------------------------ */

int
rtw8189f_power_on_check(struct rtw8189f_softc *sc)
{
	uint8_t b0, b1, b2, b3;
	uint32_t val_mix, res;
	int retry;

	b0 = rtw8189f_mac_read_1(sc, RTW8189F_REG_CR + 0);
	b1 = rtw8189f_mac_read_1(sc, RTW8189F_REG_CR + 1);
	b2 = rtw8189f_mac_read_1(sc, RTW8189F_REG_CR + 2);
	b3 = rtw8189f_mac_read_1(sc, RTW8189F_REG_CR + 3);
	if (b0 == 0xea || b1 == 0xea || b2 == 0xea || b3 == 0xea) {
		aprint_error_dev(sc->sc_dev, "power on failed (0x100=EA)\n");
		return EIO;
	}

	val_mix = b3 << 24 | b2 << 16 | b1 << 8 | b0;

	/* CMD53 dword read must agree with the CMD52 bytes. */
	for (retry = 0; retry < 100; retry++) {
		res = rtw8189f_mac_read_4(sc, RTW8189F_REG_CR);
		if (res == val_mix)
			break;
	}
	if (res != val_mix) {
		aprint_error_dev(sc->sc_dev,
		    "power on check failed: cmd52 %08x != cmd53 %08x\n",
		    val_mix, res);
		return EIO;
	}

	/* 0x1B8 write/read-back test through the CMD53 path. */
	for (retry = 0; retry < 100; retry++) {
		rtw8189f_mac_write_4(sc, 0x1b8, 0x12345678);
		res = rtw8189f_mac_read_4(sc, 0x1b8);
		if (res == 0x12345678)
			break;
	}
	if (res != 0x12345678) {
		aprint_error_dev(sc->sc_dev,
		    "power on check failed: 0x1b8 wrote 12345678 read %08x\n",
		    res);
		return EIO;
	}

	aprint_normal_dev(sc->sc_dev,
	    "power on check pass (0x100 val_mix 0x%08x)\n", val_mix);
	return 0;
}

/* ------------------------------------------------------------------ */
/* eFuse                                                               */
/* ------------------------------------------------------------------ */

static uint8_t
rtw8189f_efuse_read_1(struct rtw8189f_softc *sc, uint16_t addr)
{
	uint32_t reg;
	int retry;

	reg = rtw8189f_mac_read_4(sc, RTW8189F_REG_EFUSE_CTRL);
	reg &= ~RTW8189F_EFUSE_CTRL_ADDR_M;
	reg |= addr << RTW8189F_EFUSE_CTRL_ADDR_S;
	reg &= ~RTW8189F_EFUSE_CTRL_VALID;
	reg |= RTW8189F_EFUSE_CTRL_LDOEEN;	/* keep the eFuse LDO up */
	rtw8189f_mac_write_4(sc, RTW8189F_REG_EFUSE_CTRL, reg);

	for (retry = 0; retry < 100; retry++) {
		reg = rtw8189f_mac_read_4(sc, RTW8189F_REG_EFUSE_CTRL);
		if (reg & RTW8189F_EFUSE_CTRL_VALID)
			return reg & RTW8189F_EFUSE_CTRL_DATA_M;
		DELAY(5);
	}

	aprint_error_dev(sc->sc_dev,
	    "could not read efuse byte at 0x%04x (ctrl 0x%08x)\n",
	    addr, reg);
	return 0xff;
}

static void
rtw8189f_efuse_switch_power(struct rtw8189f_softc *sc)
{
	uint16_t reg;

	reg = rtw8189f_mac_read_2(sc, RTW8189F_REG_SYS_ISO_CTRL);
	if ((reg & RTW8189F_PWC_EV12V) == 0)
		rtw8189f_mac_write_2(sc, RTW8189F_REG_SYS_ISO_CTRL,
		    reg | RTW8189F_PWC_EV12V);

	reg = rtw8189f_mac_read_2(sc, RTW8189F_REG_SYS_FUNC_EN);
	if ((reg & RTW8189F_FEN_ELDR) == 0)
		rtw8189f_mac_write_2(sc, RTW8189F_REG_SYS_FUNC_EN,
		    reg | RTW8189F_FEN_ELDR);

	reg = rtw8189f_mac_read_2(sc, RTW8189F_REG_SYS_CLKR);
	if ((reg & (RTW8189F_LOADER_EN | RTW8189F_ANA8M)) !=
	    (RTW8189F_LOADER_EN | RTW8189F_ANA8M))
		rtw8189f_mac_write_2(sc, RTW8189F_REG_SYS_CLKR,
		    reg | RTW8189F_LOADER_EN | RTW8189F_ANA8M);
}

int
rtw8189f_efuse_read(struct rtw8189f_softc *sc)
{
	uint8_t *map = sc->sc_efuse_map;
	uint16_t addr;
	uint8_t hdr, off, msk;
	size_t i;
	int error;

	memset(map, 0xff, RTW8189F_HWSET_MAX_SIZE);

	rtw8189f_efuse_switch_power(sc);

	DNPRINTF(sc, RTW8189F_DBG_EFUSE, "efuse phys[0..3]: "
	    "%02x %02x %02x %02x (ctrl 0x%08x)\n",
	    rtw8189f_efuse_read_1(sc, 0), rtw8189f_efuse_read_1(sc, 1),
	    rtw8189f_efuse_read_1(sc, 2), rtw8189f_efuse_read_1(sc, 3),
	    rtw8189f_mac_read_4(sc, RTW8189F_REG_EFUSE_CTRL));

	/* Read the physical map (256 bytes) and parse the pg packets.
	 * RTL8188F uses the extended-header format: a packet header with
	 * (hdr & 0x1f) == 0x0f is followed by an extension byte carrying
	 * the high offset bits (logical map is 512 bytes, not 256). */
	addr = 0;
	while (addr < RTW8189F_EFUSE_REAL_CONTENT_LEN) {
		hdr = rtw8189f_efuse_read_1(sc, addr++);
		if (hdr == 0xff)
			break;
		if ((hdr & 0x1f) == 0x0f) {
			uint8_t ext = rtw8189f_efuse_read_1(sc, addr++);
			if (ext == 0xff)
				break;
			off = ((ext & 0xf0) >> 1) | ((hdr & 0xe0) >> 5);
			msk = ext & 0x0f;
		} else {
			off = (hdr >> 4) & 0x0f;
			msk = hdr & 0x0f;
		}
		for (i = 0; i < 4; i++) {
			if (msk & (1U << i))
				continue;
			if (off * 8 + i * 2 + 1 >= RTW8189F_HWSET_MAX_SIZE) {
				aprint_error_dev(sc->sc_dev,
				    "efuse header out of range\n");
				error = EIO;
				goto out;
			}
			map[off * 8 + i * 2 + 0] =
			    rtw8189f_efuse_read_1(sc, addr++);
			map[off * 8 + i * 2 + 1] =
			    rtw8189f_efuse_read_1(sc, addr++);
		}
	}

	DNPRINTF(sc, RTW8189F_DBG_EFUSE,
	    "efuse parse stopped at phys 0x%03x; logical[0x110..0x12f]: "
	    "%02x %02x %02x %02x %02x %02x %02x %02x "
	    "%02x %02x %02x %02x %02x %02x %02x %02x "
	    "%02x %02x %02x %02x %02x %02x %02x %02x "
	    "%02x %02x %02x %02x %02x %02x %02x %02x\n",
	    addr,
	    map[0x110], map[0x111], map[0x112], map[0x113],
	    map[0x114], map[0x115], map[0x116], map[0x117],
	    map[0x118], map[0x119], map[0x11a], map[0x11b],
	    map[0x11c], map[0x11d], map[0x11e], map[0x11f],
	    map[0x120], map[0x121], map[0x122], map[0x123],
	    map[0x124], map[0x125], map[0x126], map[0x127],
	    map[0x128], map[0x129], map[0x12a], map[0x12b],
	    map[0x12c], map[0x12d], map[0x12e], map[0x12f]);

	DNPRINTF(sc, RTW8189F_DBG_EFUSE, "MAC %02x:%02x:%02x:%02x:%02x:%02x "
	    "at 0x%03x\n",
	    map[RTW8189F_EEPROM_MAC_ADDR + 0], map[RTW8189F_EEPROM_MAC_ADDR + 1],
	    map[RTW8189F_EEPROM_MAC_ADDR + 2], map[RTW8189F_EEPROM_MAC_ADDR + 3],
	    map[RTW8189F_EEPROM_MAC_ADDR + 4], map[RTW8189F_EEPROM_MAC_ADDR + 5],
	    RTW8189F_EEPROM_MAC_ADDR);

	/* A valid MAC has the locally-administered bit pattern the vendor
	 * modules ship with; reject all-ff/all-00 (autoload fail). */
	memcpy(sc->sc_mac_addr, map + RTW8189F_EEPROM_MAC_ADDR,
	    IEEE80211_ADDR_LEN);
	if ((sc->sc_mac_addr[0] == 0xff && sc->sc_mac_addr[5] == 0xff) ||
	    (sc->sc_mac_addr[0] == 0x00 && sc->sc_mac_addr[5] == 0x00)) {
		aprint_error_dev(sc->sc_dev, "no valid eFuse MAC\n");
		error = ENOENT;
		goto out;
	}
	sc->sc_mac_valid = true;
	error = 0;
out:
	return error;
}

/* ------------------------------------------------------------------ */
/* Firmware download                                                   */
/* ------------------------------------------------------------------ */

static void
rtw8189f_8051_reset(struct rtw8189f_softc *sc)
{
	uint8_t io, cpu;

	/* Toggle the MCU IO wrapper (0x1c[8]) and CPU enable (0x02[10]). */
	io = rtw8189f_mac_read_1(sc, RTW8189F_REG_RSV_CTRL + 1);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_RSV_CTRL + 1, io & ~__BIT(0));

	cpu = rtw8189f_mac_read_1(sc, RTW8189F_REG_SYS_FUNC_EN + 1);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_SYS_FUNC_EN + 1,
	    cpu & ~__BIT(2));

	io = rtw8189f_mac_read_1(sc, RTW8189F_REG_RSV_CTRL + 1);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_RSV_CTRL + 1, io | __BIT(0));

	cpu = rtw8189f_mac_read_1(sc, RTW8189F_REG_SYS_FUNC_EN + 1);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_SYS_FUNC_EN + 1,
	    cpu | __BIT(2));

	DNPRINTF(sc, RTW8189F_DBG_FW, "8051 reset done\n");
}

static int
rtw8189f_fw_download_enable(struct rtw8189f_softc *sc, bool enable)
{
	uint8_t tmp;
	int retry;

	if (enable) {
		/* 8051 enable: 0x03 |= 0x04 */
		tmp = rtw8189f_mac_read_1(sc, RTW8189F_REG_SYS_FUNC_EN + 1);
		rtw8189f_mac_write_1(sc, RTW8189F_REG_SYS_FUNC_EN + 1,
		    tmp | 0x04);

		/* MCUFWDL.EN: set and poll until it sticks. */
		tmp = rtw8189f_mac_read_1(sc, RTW8189F_REG_MCUFWDL);
		rtw8189f_mac_write_1(sc, RTW8189F_REG_MCUFWDL, tmp | 0x01);
		for (retry = 0; retry < 100; retry++) {
			tmp = rtw8189f_mac_read_1(sc, RTW8189F_REG_MCUFWDL);
			if (tmp & 0x01)
				break;
			rtw8189f_mac_write_1(sc, RTW8189F_REG_MCUFWDL,
			    tmp | 0x01);
			kpause("rtw8189ffw", false, 1, NULL);
		}

		/* 8051 reset: 0x82 &= ~0x08 */
		tmp = rtw8189f_mac_read_1(sc, RTW8189F_REG_MCUFWDL + 2);
		rtw8189f_mac_write_1(sc, RTW8189F_REG_MCUFWDL + 2,
		    tmp & ~0x08);
	} else {
		tmp = rtw8189f_mac_read_1(sc, RTW8189F_REG_MCUFWDL);
		rtw8189f_mac_write_1(sc, RTW8189F_REG_MCUFWDL, tmp & ~0x01);
	}

	return 0;
}

static int
rtw8189f_fw_write_page(struct rtw8189f_softc *sc, uint8_t page,
    const uint8_t *buf, uint32_t size)
{
	uint32_t i;
	uint8_t tmp;

	/* Select the 4 KiB RAM page in MCUFWDL[2] bits [2:0]. */
	tmp = rtw8189f_mac_read_1(sc, RTW8189F_REG_MCUFWDL + 2);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_MCUFWDL + 2,
	    (tmp & 0xf8) | (page & 0x07));

	/* 4-byte IO writes to FW_START_ADDRESS, byte writes for the tail. */
	for (i = 0; i + 4 <= size; i += 4) {
		uint32_t v = le32dec(buf + i);
		rtw8189f_mac_write_4(sc,
		    RTW8189F_FW_START_ADDRESS + i, v);
	}
	for (; i < size; i++)
		rtw8189f_mac_write_1(sc, RTW8189F_FW_START_ADDRESS + i,
		    buf[i]);

	return 0;
}

int
rtw8189f_fw_download(struct rtw8189f_softc *sc)
{
	const uint8_t *fw = sc->sc_fw;
	const uint8_t *buf;
	uint32_t len, off, page;
	uint16_t sig;
	uint8_t tmp;
	int attempt, wait, error;

	if (sc->sc_fw == NULL || sc->sc_fwsize < RTW8189F_FW_HDR_SIZE + 4 ||
	    sc->sc_fwsize > RTW8189F_FW_MAX_SIZE + RTW8189F_FW_HDR_SIZE)
		return EINVAL;

	/* Download-power-state handshake: 0xA3[2:0] = 010, 0xA0[1:0] = 01. */
	tmp = rtw8189f_mac_read_1(sc, 0xa3);
	tmp = (tmp & 0xf8) | 0x02;
	rtw8189f_mac_write_1(sc, 0xa3, tmp);
	tmp = rtw8189f_mac_read_1(sc, 0xa0) & 0x03;
	if (tmp != 0x01)
		aprint_error_dev(sc->sc_dev,
		    "unexpected FW download power state 0xa0=%x\n", tmp);

	sig = le16dec(fw);
	if (sig != RTW8189F_FW_SIGNATURE) {
		aprint_error_dev(sc->sc_dev,
		    "bad firmware signature 0x%04x\n", sig);
		return EINVAL;
	}
	aprint_normal_dev(sc->sc_dev,
	    "firmware v%u, %zu bytes (header included)\n",
	    fw[RTW8189F_FW_VERSION_OFF],
	    sc->sc_fwsize);

	/* Skip the 32-byte image header. */
	buf = fw + RTW8189F_FW_HDR_SIZE;
	len = sc->sc_fwsize - RTW8189F_FW_HDR_SIZE;

	/* If 8051 is running from RAM, tell it to reset. */
	if (rtw8189f_mac_read_1(sc, RTW8189F_REG_MCUFWDL) &
	    RTW8189F_RAM_DL_SEL) {
		rtw8189f_mac_write_1(sc, RTW8189F_REG_MCUFWDL, 0x00);
		rtw8189f_8051_reset(sc);
	}

	error = EIO;
	for (attempt = 0; attempt < 3; attempt++) {
		rtw8189f_fw_download_enable(sc, true);

		/* Reset the FWDL checksum bit so the MCU re-reports. */
		tmp = rtw8189f_mac_read_1(sc, RTW8189F_REG_MCUFWDL);
		rtw8189f_mac_write_1(sc, RTW8189F_REG_MCUFWDL,
		    tmp | RTW8189F_FWDL_CHKSUM_RPT);

		for (page = 0, off = 0; off < len;
		    page++, off += RTW8189F_FW_DL_PAGE_SIZE) {
			uint32_t n =
			    MIN(len - off, RTW8189F_FW_DL_PAGE_SIZE);
			rtw8189f_fw_write_page(sc, page, buf + off, n);
		}

		rtw8189f_fw_download_enable(sc, false);

		/* Poll the checksum report: MCUFWDL.BIT2 within ~250ms. */
		for (wait = 0; wait < 250; wait++) {
			uint32_t v =
			    rtw8189f_mac_read_4(sc, RTW8189F_REG_MCUFWDL);
			if (v & RTW8189F_FWDL_CHKSUM_RPT)
				break;
			kpause("rtw8189fc", false, 1, NULL);
		}
		if (wait < 250)
			break;
		aprint_error_dev(sc->sc_dev,
		    "firmware checksum not reported (attempt %d)\n",
		    attempt + 1);
	}
	if (attempt == 3)
		goto fail;

	aprint_normal_dev(sc->sc_dev, "firmware download checksum OK\n");

	return rtw8189f_fw_ready(sc);

fail:
	return error;
}

int
rtw8189f_fw_ready(struct rtw8189f_softc *sc)
{
	uint32_t expected = RTW8189F_MCUFWDL_RDY | RTW8189F_FWDL_CHKSUM_RPT |
	    RTW8189F_WINTINI_RDY | RTW8189F_RAM_DL_SEL;
	uint32_t v;
	int retry;

	/* MCUFWDL.RDY=1, clear WINTINI, reset the 8051, poll for boot. */
	v = rtw8189f_mac_read_4(sc, RTW8189F_REG_MCUFWDL);
	v |= RTW8189F_MCUFWDL_RDY;
	v &= ~RTW8189F_WINTINI_RDY;
	rtw8189f_mac_write_4(sc, RTW8189F_REG_MCUFWDL, v);

	rtw8189f_8051_reset(sc);

	for (retry = 0; retry < 200; retry++) {
		v = rtw8189f_mac_read_4(sc, RTW8189F_REG_MCUFWDL);
		if ((v & expected) == expected) {
			sc->sc_fw_ready = true;
			aprint_normal_dev(sc->sc_dev,
			    "firmware ready (MCUFWDL 0x%08x, %d ms)\n",
			    v, retry);
			return 0;
		}
		kpause("rtw8189fr", false, 1, NULL);
	}

	aprint_error_dev(sc->sc_dev,
	    "firmware not ready (MCUFWDL 0x%08x)\n", v);
	return EIO;
}

/* ------------------------------------------------------------------ */
/* RF (LSSI) access                                                    */
/* ------------------------------------------------------------------ */

/*
 * RF writes go through the BB "3-wire" register 0x840 with the register
 * index in bits [27:20] and the 20-bit data below (phy_RFSerialWrite_
 * 8188F).  Reads set the index in HSSIParameter2 (0x824), toggle the
 * read edge and sample the readback register (phy_RFSerialRead_8188F).
 */
static void
rtw8189f_rf_write20(struct rtw8189f_softc *sc, uint32_t off, uint32_t data)
{
	uint32_t v = (((off & 0xff) << 20) | (data & RTW8189F_LSSI_READBACK_M))
	    & 0x0fffffff;

	rtw8189f_mac_write_4(sc, RTW8189F_BB_LSSI_WRITE, v);
	delay(1);
}

static uint32_t
rtw8189f_rf_read20(struct rtw8189f_softc *sc, uint32_t off)
{
	uint32_t t, v;
	int pi;

	t = rtw8189f_mac_read_4(sc, RTW8189F_BB_HSSI_P2);
	t = (t & ~RTW8189F_LSSI_READ_ADDR_M) |
	    ((off & 0xff) << 23) | RTW8189F_LSSI_READ_EDGE;
	rtw8189f_mac_write_4(sc, RTW8189F_BB_HSSI_P2, t & ~RTW8189F_LSSI_READ_EDGE);
	t = rtw8189f_mac_read_4(sc, RTW8189F_BB_HSSI_P2);
	rtw8189f_mac_write_4(sc, RTW8189F_BB_HSSI_P2, t & ~RTW8189F_LSSI_READ_EDGE);
	rtw8189f_mac_write_4(sc, RTW8189F_BB_HSSI_P2, t | RTW8189F_LSSI_READ_EDGE);

	delay(10);
	delay(50);
	delay(50);
	delay(10);

	pi = rtw8189f_mac_read_4(sc, RTW8189F_BB_HSSI_P1) & __BIT(8);
	v = rtw8189f_mac_read_4(sc, pi ? RTW8189F_BB_HSPI_READBACK
					: RTW8189F_BB_LSSI_READBACK);
	return v & RTW8189F_LSSI_READBACK_M;
}

/*
 * Vendor-style masked access (PHY_SetBBReg_8188F / PHY_SetRFReg_8188F):
 * "data" fills the field selected by "mask" and is shifted down to the
 * mask's lowest set bit ((data << ctz(mask)) & mask).  The IQK sequences
 * are transcribed call by call in this convention.
 */
static uint32_t
rtw8189f_bb_get(struct rtw8189f_softc *sc, uint32_t addr, uint32_t mask)
{

	return (rtw8189f_mac_read_4(sc, addr) & mask) >> __builtin_ctz(mask);
}

static void
rtw8189f_bb_set(struct rtw8189f_softc *sc, uint32_t addr, uint32_t mask,
    uint32_t data)
{
	uint32_t v;

	if (mask != 0xffffffff) {
		v = rtw8189f_mac_read_4(sc, addr);
		data = (v & ~mask) | ((data << __builtin_ctz(mask)) & mask);
	}
	rtw8189f_mac_write_4(sc, addr, data);
}

static void
rtw8189f_rf_rmw20(struct rtw8189f_softc *sc, uint32_t off, uint32_t mask,
    uint32_t data)
{
	uint32_t v = rtw8189f_rf_read20(sc, off);

	rtw8189f_rf_write20(sc, off,
	    (v & ~mask) | ((data << __builtin_ctz(mask)) & mask));
}

/* ------------------------------------------------------------------ */
/* Init table interpreter                                              */
/* ------------------------------------------------------------------ */

/*
 * The vendor tables carry IF/ELSE/ENDIF blocks in address words with
 * BIT31/BIT30 set.  Condition matching is a bit-defined compare against
 * the driver identity vector (check_positive): we replicate the exact
 * identity the Linux driver computes -- cut A counts as 15 (unknown),
 * platform ODM_CE = 4, interface ODM_ITRF_SDIO = 4, package unknown
 * counts as 15 -- so the same branches are taken.
 */
#define RTW8189F_DRIVER1	0x0f04f400

static bool
rtw8189f_cond_match(uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4)
{
	uint32_t driver1 = RTW8189F_DRIVER1, bit_mask = 0;
	uint32_t driver2 = 0, driver4 = 0;	/* no RF-path type selects */

	(void)c3;
	if (((c1 & 0x0000f000) != 0) &&
	    ((c1 & 0x0000f000) != (driver1 & 0x0000f000)))
		return false;
	if (((c1 & 0x0f000000) != 0) &&
	    ((c1 & 0x0f000000) != (driver1 & 0x0f000000)))
		return false;

	c1 &= 0x00ff0fff;
	driver1 &= 0x00ff0fff;
	if ((c1 & driver1) != c1)
		return false;

	if ((c1 & 0x0f) == 0)	/* board type is DONTCARE */
		return true;

	if (c1 & __BIT(0))
		bit_mask |= 0x000000ff;
	if (c1 & __BIT(1))
		bit_mask |= 0x0000ff00;
	if (c1 & __BIT(2))
		bit_mask |= 0x00ff0000;
	if (c1 & __BIT(3))
		bit_mask |= 0xff000000;

	return ((c2 & bit_mask) == (driver2 & bit_mask)) &&
	       ((c4 & bit_mask) == (driver4 & bit_mask));
}

#define RTW8189F_COND_ENDIF	3
#define RTW8189F_COND_ELSE	2

/*
 * Shared per-table walk: drives the condition state machine and hands
 * matched {addr, val} pairs to the apply callback.  Returns the number
 * of applied entries.
 */
static int
rtw8189f_walk_table(const uint32_t *tbl, size_t npairs,
    int (*apply)(struct rtw8189f_softc *, uint32_t, uint32_t, void *),
    struct rtw8189f_softc *sc, void *arg)
{
	bool matched = true, skipped = false;
	uint32_t pre1 = 0, pre2 = 0;
	size_t i;
	int applied = 0;

	for (i = 0; i < npairs; i++) {
		uint32_t v1 = tbl[2 * i], v2 = tbl[2 * i + 1];

		if (v1 & (__BIT(31) | __BIT(30))) {
			if (v1 & __BIT(31)) {
				uint8_t c_cond = (v1 & (__BIT(29) | __BIT(28))) >> 28;

				if (c_cond == RTW8189F_COND_ENDIF) {
					matched = true;
					skipped = false;
				} else if (c_cond == RTW8189F_COND_ELSE) {
					matched = !skipped;
				} else {
					pre1 = v1;
					pre2 = v2;
				}
			} else {	/* negative condition body */
				if (!skipped) {
					if (rtw8189f_cond_match(pre1, pre2, v1, v2)) {
						matched = true;
						skipped = true;
					} else {
						matched = false;
						skipped = false;
					}
				} else
					matched = false;
			}
		} else {
			if (matched)
				applied += apply(sc, v1, v2, arg) ? 1 : 0;
		}
	}
	return applied;
}

static int
rtw8189f_apply_mac(struct rtw8189f_softc *sc, uint32_t addr, uint32_t val,
    void *arg)
{
	rtw8189f_mac_write_1(sc, addr, val & 0xff);
	return 1;
}

static int
rtw8189f_apply_bb(struct rtw8189f_softc *sc, uint32_t addr, uint32_t val,
    void *arg)
{
	/* 0xF9..0xFE are delay markers; none appear in the 8188F images. */
	if (addr >= 0xf9 && addr <= 0xfe)
		return 0;
	rtw8189f_mac_write_4(sc, addr, val);
	return 1;
}

static int
rtw8189f_apply_rf(struct rtw8189f_softc *sc, uint32_t addr, uint32_t val,
    void *arg)
{
	uint32_t get;
	uint8_t count;

	/* RF tables use 0xfe/0xffe as 50 ms delays, notably between
	 * the LCK trigger and the RF1f calibration latch sequence. */
	if (addr == 0xfe || addr == 0xffe) {
		kpause("rtw8189frf", false, mstohz(50), NULL);
		return 0;
	}
	rtw8189f_rf_write20(sc, addr, val);

	if (addr == 0xb6) {
		for (count = 0; count < 6; count++) {
			get = rtw8189f_rf_read20(sc, addr);
			if ((get >> 8) == (val >> 8))
				break;
			rtw8189f_rf_write20(sc, addr, val);
		}
	} else if (addr == 0xb2) {
		for (count = 0; count < 6; count++) {
			get = rtw8189f_rf_read20(sc, addr);
			if (get == val)
				break;
			rtw8189f_rf_write20(sc, addr, val);
			/* Redo the LC calibration. */
			rtw8189f_rf_write20(sc, RTW8189F_RF_CHNLBW, 0x0fc07);
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Chip init (vendor rtl8188fs_hal_init, post-firmware section)        */
/* ------------------------------------------------------------------ */

int
rtw8189f_chip_init(struct rtw8189f_softc *sc)
{
	uint32_t v;
	uint8_t v8;
	unsigned lc_retry;

	/* 1. MAC init table (1-byte register writes). */
	rtw8189f_walk_table(rtw8189f_mac_tbl, __arraycount(rtw8189f_mac_tbl) / 2,
	    rtw8189f_apply_mac, sc, NULL);

	/* 2. BB config: enable the BB resets/clock, RF on, RF register 1,
	 * then the PHY and AGC tables (PHY_BBConfig8188F). */
	v = rtw8189f_mac_read_2(sc, RTW8189F_REG_SYS_FUNC_EN);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_SYS_FUNC_EN,
	    v | __BIT(13) | __BIT(1) | __BIT(0));
	rtw8189f_mac_write_1(sc, 0x1f, __BIT(0) | __BIT(1) | __BIT(2));
	delay(10);
	rtw8189f_rf_write20(sc, 0x1, 0x780);

	rtw8189f_walk_table(rtw8189f_bb_phy_tbl,
	    __arraycount(rtw8189f_bb_phy_tbl) / 2, rtw8189f_apply_bb, sc, NULL);
	rtw8189f_walk_table(rtw8189f_bb_agc_tbl,
	    __arraycount(rtw8189f_bb_agc_tbl) / 2, rtw8189f_apply_bb, sc, NULL);

	/* 3. RF init table (with the 0xb6/0xb2 readback-verified entries). */
	rtw8189f_walk_table(rtw8189f_rf_tbl,
	    __arraycount(rtw8189f_rf_tbl) / 2, rtw8189f_apply_rf, sc, NULL);

	/*
	 * Vendor halrf_lck_trigger: assert RF18 bit15, poll until the
	 * hardware self-clears it, then restore the channel image.  A
	 * settle delay comes before the first poll -- the trigger write
	 * needs time to latch, and a single immediate read can observe the
	 * pre-calibration value and end the wait before LCK even starts.
	 */
	{
		uint32_t lc_cal = rtw8189f_rf_read20(sc, RTW8189F_RF_CHNLBW);

		rtw8189f_rf_write20(sc, RTW8189F_RF_CHNLBW, lc_cal | __BIT(15));
		kpause("rtw8189fl0", false, mstohz(10), NULL);
		for (lc_retry = 0; lc_retry < 100; lc_retry++) {
			v = rtw8189f_rf_read20(sc, RTW8189F_RF_CHNLBW);
			if ((v & __BIT(15)) == 0)
				break;
			kpause("rtw8189flc", false, mstohz(10), NULL);
		}
		if (lc_retry == 100) {
			aprint_error_dev(sc->sc_dev,
			    "initial LC calibration timed out\n");
			return ETIMEDOUT;
		}
		sc->sc_rf18 = rtw8189f_rf_read20(sc, RTW8189F_RF_CHNLBW);
		DNPRINTF(sc, RTW8189F_DBG_INIT,
		    "lck: cal 0x%05x settled 0x%05x after %u ms\n",
		    lc_cal, sc->sc_rf18, 10 + lc_retry * 10);
	}

	/* 4. TX page allocation: NPQ/HPQ/LPQ/PUB + buffer boundaries. */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_RQPN_NPQ, RTW8189F_RQPN_NPQ(RTW8189F_PAGE_NUM_NPQ));
	rtw8189f_mac_write_4(sc, RTW8189F_REG_RQPN,
	    RTW8189F_RQPN_HPQ(RTW8189F_PAGE_NUM_HPQ) |
	    RTW8189F_RQPN_LPQ(RTW8189F_PAGE_NUM_LPQ) |
	    RTW8189F_RQPN_PUBQ(RTW8189F_NUM_PUBQ) |
	    RTW8189F_RQPN_LD_RQPN);

	rtw8189f_mac_write_1(sc, RTW8189F_REG_TXPKTBUF_BCNQ_BDNY, RTW8189F_TX_PAGE_BOUNDARY);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_TXPKTBUF_MGQ_BDNY, RTW8189F_TX_PAGE_BOUNDARY);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_TXPKTBUF_WMAC_LBK_BF_HD, RTW8189F_TX_PAGE_BOUNDARY);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_TRXFF_BNDY, RTW8189F_TX_PAGE_BOUNDARY);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_TDECTRL + 1, RTW8189F_TX_PAGE_BOUNDARY);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_TRXFF_BNDY + 2, RTW8189F_RX_DMA_BOUNDARY);

	/* 5. Auto LLT. */
	v = rtw8189f_mac_read_4(sc, RTW8189F_REG_AUTO_LLT);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_AUTO_LLT, v | RTW8189F_BIT_AUTO_INIT_LLT);
	{
		int retry;

		for (retry = 0; retry < 1000; retry++) {
			v = rtw8189f_mac_read_4(sc, RTW8189F_REG_AUTO_LLT);
			if ((v & RTW8189F_BIT_AUTO_INIT_LLT) == 0)
				break;
			kpause("rtw8189fl", false, 1, NULL);
		}
		if (v & RTW8189F_BIT_AUTO_INIT_LLT) {
			aprint_error_dev(sc->sc_dev, "auto LLT timeout\n");
			return EIO;
		}
	}

	/* 6. Queue-to-TXDMA mapping (three-out-EP default: BE/BK low,
	 * VI normal, VO/MG/HI high).  RX aggregation stays off. */
	v = rtw8189f_mac_read_2(sc, RTW8189F_REG_TRXDMA_CTRL) & 0x7;
	v |= RTW8189F_TRXDMA_BEQ_MAP(RTW8189F_QUEUE_LOW) |
	     RTW8189F_TRXDMA_BKQ_MAP(RTW8189F_QUEUE_LOW) |
	     RTW8189F_TRXDMA_VIQ_MAP(RTW8189F_QUEUE_NORMAL) |
	     RTW8189F_TRXDMA_VOQ_MAP(RTW8189F_QUEUE_HIGH) |
	     RTW8189F_TRXDMA_MGQ_MAP(RTW8189F_QUEUE_HIGH) |
	     RTW8189F_TRXDMA_HIQ_MAP(RTW8189F_QUEUE_HIGH);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_TRXDMA_CTRL, v);

	/* 7. Page size 128, driver info size 32 bytes. */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_PBP,
	    RTW8189F_PBP_RX(RTW8189F_PBP_128) | RTW8189F_PBP_TX(RTW8189F_PBP_128));
	rtw8189f_mac_write_1(sc, RTW8189F_REG_RX_DRVINFO_SZ, 4);

	/* 8. Network type (vendor uses NT_LINK_AP here; the state machine
	 * programs the rest). */
	v = rtw8189f_mac_read_4(sc, RTW8189F_REG_CR);
	v = (v & ~RTW8189F_CR_NETTYPE_M) | RTW8189F_CR_NETTYPE(RTW8189F_NT_LINK_AP);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_CR, v);

	/* 9. RCR: vendor default plus AAP (the MAC drops everything else'
	 * probe responses until the address filters are programmed --
	 * same lesson as the 8821CU RCR BIT_AAP).  CBSSID_BCN/DATA are
	 * dropped during scan windows only (see scan_rx_fltr). */
	sc->sc_rcr = RTW8189F_RCR_DEFAULT | RTW8189F_RCR_AAP;
	rtw8189f_mac_write_4(sc, RTW8189F_REG_RCR, sc->sc_rcr);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_MAR + 0, 0xffffffff);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_MAR + 4, 0xffffffff);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_RXFLTMAP2, 0xffff);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_RXFLTMAP1, 0x400);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_RXFLTMAP0, 0xffff);

	/* 10. Response rates, SIFS, retry limits, EDCA. */
	v = rtw8189f_mac_read_4(sc, RTW8189F_REG_RRSR);
	v &= ~0xfffff;
	v |= RTW8189F_RATE_RRSR_CCK_ONLY_1M;
	rtw8189f_mac_write_4(sc, RTW8189F_REG_RRSR, v);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_SPEC_SIFS, 0x100a);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_MAC_SPEC_SIFS, 0x100a);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_SIFS_CTX, 0x100a);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_SIFS_TRX, 0x100a);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_RETRY_LIMIT, RTW8189F_RETRY_LIMIT(0x30));
	rtw8189f_mac_write_4(sc, RTW8189F_REG_EDCA_BE_PARAM, 0x005ea42b);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_EDCA_BK_PARAM, 0x0000a44f);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_EDCA_VI_PARAM, 0x005ea324);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_EDCA_VO_PARAM, 0x002fa226);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_USTIME_EDCA, 0x28);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_USTIME_TSF, 0x28);

	/* 11. Retry function + ACK timeout. */
	v8 = rtw8189f_mac_read_1(sc, RTW8189F_REG_FWHW_TXQ_CTRL);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_FWHW_TXQ_CTRL,
	    v8 | RTW8189F_AMPDU_RTY_NEW);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_ACKTO, 0x40);

	/* 12. Beacon parameters (STA: TSF update disabled). */
	rtw8189f_mac_write_2(sc, RTW8189F_REG_BCN_CTRL,
	    RTW8189F_BCN_DIS_TSF_UDT | (RTW8189F_BCN_DIS_TSF_UDT << 8));
	rtw8189f_mac_write_1(sc, RTW8189F_REG_TBTT_PROHIBIT, 0x04);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_TBTT_PROHIBIT + 1, 0x64);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_BCNDMATIM, 0x02);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_BCNTCFG, 0x4413);

	/* 13. Burst/single-pkt misc (vendor _InitBurstPktLen_8188FS). */
	v8 = rtw8189f_mac_read_1(sc, 0x4c7);
	rtw8189f_mac_write_1(sc, 0x4c7, v8 | __BIT(7));
	rtw8189f_mac_write_1(sc, RTW8189F_REG_RX_PKT_LIMIT, 0x18);
	rtw8189f_mac_write_1(sc, 0x4ca, 0x1f);		/* MAX_AGGR_NUM */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_PIFS, 0x00);
	v8 = rtw8189f_mac_read_1(sc, RTW8189F_REG_FWHW_TXQ_CTRL);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_FWHW_TXQ_CTRL, v8 & ~__BIT(7));
	rtw8189f_mac_write_1(sc, RTW8189F_REG_AMPDU_MAX_TIME, 0x70);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_ARFR0, 0x00000010);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_ARFR0 + 4, 0xfffff000);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_ARFR1, 0x00000010);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_ARFR1 + 4, 0x003ff000);

	/* 14. Secondary CCA + BAR mode + HW sequence numbers. */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_SECONDARY_CCA_CTRL, 0x3);
	rtw8189f_mac_write_1(sc, 0x976, 0);
	rtw8189f_mac_write_4(sc, RTW8189F_REG_BAR_MODE_CTRL, 0x0201ffff);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_HWSEQ_CTRL, 0xff);

	/* 15. SDIO TX control: keep 0x0[15:3], clear the rest. */
	{
		uint32_t t;

		t  = (uint32_t)rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 0);
		t |= (uint32_t)rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 1) << 8;
		t |= (uint32_t)rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 2) << 16;
		t |= (uint32_t)rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 3) << 24;
		t &= 0x0000fff8;
		rtw8189f_sdiolocal_write_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 0, t & 0xff);
		rtw8189f_sdiolocal_write_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 1, (t >> 8) & 0xff);
		rtw8189f_sdiolocal_write_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 2, 0);
		rtw8189f_sdiolocal_write_1(sc, RTW8189F_SDIO_REG_TX_CTRL + 3, 0);
	}

	/* 16. Turn on CCK/OFDM blocks. */
	v = rtw8189f_mac_read_4(sc, RTW8189F_BB_RFMOD);
	rtw8189f_mac_write_4(sc, RTW8189F_BB_RFMOD,
	    v | RTW8189F_BB_RFMOD_CCK_EN | RTW8189F_BB_RFMOD_OFDM_EN);

	/* 20 MHz BB clock/filter configuration (RTL8188F). */
	v = rtw8189f_mac_read_4(sc, 0x800);
	rtw8189f_mac_write_4(sc, 0x800,
	    (v & ~0x7701U) | 0x5700);
	v = rtw8189f_mac_read_4(sc, 0x900);
	rtw8189f_mac_write_4(sc, 0x900, v & ~1U);
	v = rtw8189f_mac_read_4(sc, 0xce4);
	rtw8189f_mac_write_4(sc, 0xce4, (v & ~0xf0000000U) | 0x10000000);
	v = rtw8189f_mac_read_4(sc, 0xc10);
	rtw8189f_mac_write_4(sc, 0xc10, (v & ~0x30000000U) | 0x10000000);
	v = rtw8189f_mac_read_4(sc, 0x954);
	rtw8189f_mac_write_4(sc, 0x954, (v & ~0x00f80000U) | 0x00300000);

	/* Crystal trim is board-specific, stored at logical eFuse 0xb9. */
	v8 = sc->sc_efuse_map[0xb9];
	if (v8 == 0xff)
		v8 = 0x20;
	v8 &= 0x3f;
	v = rtw8189f_mac_read_4(sc, 0x24);
	rtw8189f_mac_write_4(sc, 0x24,
	    (v & ~0x007ff800U) | ((v8 | (v8 << 6)) << 11));
	DNPRINTF(sc, RTW8189F_DBG_INIT, "crystal trim %u\n", v8);

	/* 17. TX power indices parsed from the eFuse PG section (with
	 * IC-default fallbacks) BEFORE the first channel switch programs
	 * them; then default channel 1 + 20 MHz RF bandwidth settings. */
	rtw8189f_txpwr_parse(sc);
	rtw8189f_rf_write20(sc, 0x87, 0x065);
	rtw8189f_rf_write20(sc, 0x1c, 0x000);
	rtw8189f_rf_write20(sc, 0xdf, 0x140);
	rtw8189f_rf_write20(sc, 0x1b, 0x0c6c);
	rtw8189f_set_channel(sc, 1);

	/* 19. Enable MAC TX/RX, NAV upper bound (30ms / 128us). */
	v8 = rtw8189f_mac_read_1(sc, RTW8189F_REG_CR);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_CR,
	    v8 | RTW8189F_MACTXEN | RTW8189F_MACRXEN);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_NAV_UPPER, (30000 + 127) / 128);

	/* Vendor CONFIG_XMIT_ACK init tail: let the firmware report
	 * management-frame TX status (C2H CCX_TX_RPT); without this bit
	 * SPE_RPT requests are silently ignored. */
	v = rtw8189f_mac_read_2(sc, RTW8189F_REG_FWHW_TXQ_CTRL);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_FWHW_TXQ_CTRL, v | __BIT(12));

	/* Vendor EnableInterrupt8188FSdio init tail: C2H events already
	 * posted before MAC RX went live would otherwise stall the
	 * firmware's C2H scheduler.  (Per-event C2H packets arrive via the
	 * RX FIFO on SDIO — CONFIG_FW_C2H_PKT — and need no mailbox
	 * handling; this is only the one-time init clear.) */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_C2HEVT_CLEAR, 0x00);

	/* 20. Our own address (MACID port 0). */
	rtw8189f_mac_write_4(sc, RTW8189F_REG_MACID,
	    (uint32_t)sc->sc_mac_addr[0] |
	    ((uint32_t)sc->sc_mac_addr[1] << 8) |
	    ((uint32_t)sc->sc_mac_addr[2] << 16) |
	    ((uint32_t)sc->sc_mac_addr[3] << 24));
	rtw8189f_mac_write_2(sc, RTW8189F_REG_MACID + 4,
	    (uint16_t)(sc->sc_mac_addr[4] | (sc->sc_mac_addr[5] << 8)));

	aprint_normal_dev(sc->sc_dev,
	    "chip init done (rf18 0x%05x, rcr 0x%08x)\n", sc->sc_rf18, sc->sc_rcr);
	/* Read back the TXAGC registers the first channel programmed. */
	DNPRINTF(sc, RTW8189F_DBG_INIT,
	    "txagc rd: cck1 %08x cck2_11 %08x ofdm6_18 %08x ofdm24_54 %08x\n",
	    rtw8189f_mac_read_4(sc, RTW8189F_TXAGC_CCK1),
	    rtw8189f_mac_read_4(sc, RTW8189F_TXAGC_CCK2_11),
	    rtw8189f_mac_read_4(sc, RTW8189F_TXAGC_OFDM6_18),
	    rtw8189f_mac_read_4(sc, RTW8189F_TXAGC_OFDM24_54));
	return 0;
}

/* ------------------------------------------------------------------ */
/* TX power (vendor hal_load_pg_txpwr_info / phy_set_tx_power_level)   */
/* ------------------------------------------------------------------ */

static int8_t
rtw8189f_txpwr_diff_nib(uint8_t v, bool msb)
{
	uint8_t n = msb ? (v >> 4) : (v & 0xf);

	/* 4-bit signed. */
	return (n & 0x8) ? (int8_t)(n | 0xf0) : (int8_t)n;
}

/*
 * Parse the TX power PG section (logical eFuse 0x10, path A) into the
 * per-group bases.  Invalid bases (> 63) fall back to the 8188F
 * IC-default table entry by entry, as the vendor's PG -> IC_DEF source
 * fallback does.
 */
static void
rtw8189f_txpwr_parse(struct rtw8189f_softc *sc)
{
	const uint8_t *pg = sc->sc_efuse_map + RTW8189F_EFUSE_TXPWR_OFF;
	unsigned g;
	int8_t ofdm_diff, bw20_diff;

	for (g = 0; g < 6; g++)
		sc->sc_txpwr_cck_base[g] =
		    pg[g] <= RTW8189F_TXPWR_MAX ? pg[g] : RTW8189F_TXPWR_DEF_CCK;
	for (g = 0; g < 5; g++)
		sc->sc_txpwr_ofdm_base[g] =
		    pg[6 + g] <= RTW8189F_TXPWR_MAX ?
		    pg[6 + g] : RTW8189F_TXPWR_DEF_OFDM;

	/* Byte 12: MSB = BW20-1T diff, LSB = OFDM-1T diff.  The 0xFF
	 * (unprogrammed) form sign-extends to -1, exactly as the vendor
	 * parser keeps it.  CCK-1T diff is never stored in PG (0). */
	bw20_diff = rtw8189f_txpwr_diff_nib(pg[11], true);
	ofdm_diff = rtw8189f_txpwr_diff_nib(pg[11], false);
	sc->sc_txpwr_bw20_diff = bw20_diff;
	sc->sc_txpwr_ofdm_diff = ofdm_diff;

	DNPRINTF(sc, RTW8189F_DBG_INIT,
	    "txpwr: cck base %02x %02x %02x %02x %02x %02x, ofdm base "
	    "%02x %02x %02x %02x %02x, diff ofdm %+d bw20 %+d\n",
	    sc->sc_txpwr_cck_base[0], sc->sc_txpwr_cck_base[1],
	    sc->sc_txpwr_cck_base[2], sc->sc_txpwr_cck_base[3],
	    sc->sc_txpwr_cck_base[4], sc->sc_txpwr_cck_base[5],
	    sc->sc_txpwr_ofdm_base[0], sc->sc_txpwr_ofdm_base[1],
	    sc->sc_txpwr_ofdm_base[2], sc->sc_txpwr_ofdm_base[3],
	    sc->sc_txpwr_ofdm_base[4], ofdm_diff, bw20_diff);
}

static unsigned
rtw8189f_txpwr_clamp(int v)
{

	if (v < 0)
		return 0;
	if (v > RTW8189F_TXPWR_MAX)
		return RTW8189F_TXPWR_MAX;
	return (unsigned)v;
}

/*
 * Program the per-rate TX power indices for one channel (vendor
 * PHY_SetTxPowerIndex_8188F; re-run on every channel switch).
 */
static void
rtw8189f_set_txpower(struct rtw8189f_softc *sc, unsigned chan)
{
	unsigned cck_g, ofdm_g, cck_idx, ofdm_idx;
	uint32_t v;

	if (chan < 1 || chan > 14)
		return;

	/* CCK groups: 1-2, 3-5, 6-8, 9-11, 12-13, 14. */
	if (chan <= 2)
		cck_g = 0;
	else if (chan <= 5)
		cck_g = 1;
	else if (chan <= 8)
		cck_g = 2;
	else if (chan <= 11)
		cck_g = 3;
	else if (chan <= 13)
		cck_g = 4;
	else
		cck_g = 5;
	/* OFDM/BW40 groups: 1-2, 3-5, 6-8, 9-11, 12-14. */
	if (chan <= 2)
		ofdm_g = 0;
	else if (chan <= 5)
		ofdm_g = 1;
	else if (chan <= 8)
		ofdm_g = 2;
	else if (chan <= 11)
		ofdm_g = 3;
	else
		ofdm_g = 4;

	cck_idx = rtw8189f_txpwr_clamp(sc->sc_txpwr_cck_base[cck_g]);
	ofdm_idx = rtw8189f_txpwr_clamp(sc->sc_txpwr_ofdm_base[ofdm_g] +
	    sc->sc_txpwr_ofdm_diff);


	/* CCK 1M (0xe08 byte1) and 2/5.5/11M (0x86c bytes1-3). */
	v = rtw8189f_mac_read_4(sc, RTW8189F_TXAGC_CCK1);
	rtw8189f_mac_write_4(sc, RTW8189F_TXAGC_CCK1,
	    (v & ~0x0000ff00) | (cck_idx << 8));
	v = rtw8189f_mac_read_4(sc, RTW8189F_TXAGC_CCK2_11);
	rtw8189f_mac_write_4(sc, RTW8189F_TXAGC_CCK2_11,
	    (v & ~0xffffff00U) | (cck_idx << 8) | (cck_idx << 16) |
	    (cck_idx << 24));

	/* OFDM 6/9/12/18M (0xe00) and 24/36/48/54M (0xe04). */
	v = ofdm_idx | (ofdm_idx << 8) | (ofdm_idx << 16) | (ofdm_idx << 24);
	rtw8189f_mac_write_4(sc, RTW8189F_TXAGC_OFDM6_18, v);
	rtw8189f_mac_write_4(sc, RTW8189F_TXAGC_OFDM24_54, v);

	DNPRINTF(sc, RTW8189F_DBG_RX, "txpwr ch %u cck %u ofdm %u\n",
	    chan, cck_idx, ofdm_idx);
}

/* ------------------------------------------------------------------ */
/* IQK: path A TX/RX I/Q imbalance calibration (vendor halrf_8188f.c)  */
/* ------------------------------------------------------------------ */

/* ADDA on/off list (_phy_path_adda_on8188f, IQK_ADDA_REG_NUM = 16). */
static const uint32_t rtw8189f_iqk_adda[16] = {
	0x85c, 0xe6c, 0xe70, 0xe74, 0xe78, 0xe7c, 0xe80, 0xe84,
	0xe88, 0xe8c, 0xed0, 0xed4, 0xed8, 0xedc, 0xee0, 0xeec
};

/* MAC registers backed up around calibration (IQK_MAC_REG). */
static const uint32_t rtw8189f_iqk_mac[4] = { 0x522, 0x550, 0x551, 0x040 };

/* BB registers backed up around calibration (IQK_BB_REG_92C). */
static const uint32_t rtw8189f_iqk_bb[9] = {
	0xc04, 0xc08, 0x874, 0xb68, 0xb6c, 0x870, 0x860, 0x864, 0x800
};

static void
rtw8189f_iqk_adda_on(struct rtw8189f_softc *sc)
{
	unsigned i;

	for (i = 0; i < 16; i++)
		rtw8189f_bb_set(sc, rtw8189f_iqk_adda[i], 0xffffffff,
		    0x03c00014);
}

static void
rtw8189f_iqk_save(struct rtw8189f_softc *sc, uint32_t *adda_bk,
    uint32_t *mac_bk, uint32_t *bb_bk)
{
	unsigned i;

	for (i = 0; i < 16; i++)
		adda_bk[i] = rtw8189f_mac_read_4(sc, rtw8189f_iqk_adda[i]);
	for (i = 0; i < 3; i++)
		mac_bk[i] = rtw8189f_mac_read_1(sc, rtw8189f_iqk_mac[i]);
	mac_bk[3] = rtw8189f_mac_read_4(sc, rtw8189f_iqk_mac[3]);
	for (i = 0; i < 9; i++)
		bb_bk[i] = rtw8189f_mac_read_4(sc, rtw8189f_iqk_bb[i]);
}

static void
rtw8189f_iqk_reload(struct rtw8189f_softc *sc, uint32_t *adda_bk,
    uint32_t *mac_bk, uint32_t *bb_bk)
{
	unsigned i;

	for (i = 0; i < 16; i++)
		rtw8189f_mac_write_4(sc, rtw8189f_iqk_adda[i], adda_bk[i]);
	for (i = 0; i < 3; i++)
		rtw8189f_mac_write_1(sc, rtw8189f_iqk_mac[i], mac_bk[i]);
	rtw8189f_mac_write_4(sc, rtw8189f_iqk_mac[3], mac_bk[3]);
	for (i = 0; i < 9; i++)
		rtw8189f_mac_write_4(sc, rtw8189f_iqk_bb[i], bb_bk[i]);
}

static void
rtw8189f_iqk_delay(struct rtw8189f_softc *sc)
{

	kpause("rtw8189fiq", false, mstohz(RTW8189F_IQK_DELAY_MS), NULL);
}

/*
 * TX IQK one-shot with LOK (vendor phy_path_a_iqk_8188f).  Returns 1 on
 * success and stores the RF 0x08 LOK result in *lokp.
 */
static uint8_t
rtw8189f_iqk_tx(struct rtw8189f_softc *sc, uint32_t *lokp)
{
	uint32_t eac, e94, e9c;

	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x000000);
	/* RF LUT: TX PA settings for the calibration tone. */
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_WE_LUT, __BIT(19), 1);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_RCK_OS, 0xfffff, 0x20000);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_TXPA_G1, 0xfffff, 0x0000f);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_TXPA_G2, 0xfffff, 0x07ff7);
	rtw8189f_rf_rmw20(sc, 0xdf, 0xfffff, 0x980);
	rtw8189f_rf_rmw20(sc, 0x56, 0xfffff, 0x5102a);

	/* Enter IQK mode. */
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x808000);

	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK_TONE_A, 0xffffffff, 0x18008c1c);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_TONE_A, 0xffffffff, 0x38008c1c);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK_PI_A, 0xffffffff, 0x821403ff);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_PI_A, 0xffffffff, 0x28160000);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_RSP, 0xffffffff, 0x00462911);
	/* One shot, path A LOK & IQK. */
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_PTS, 0xffffffff, 0xf9000000);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_PTS, 0xffffffff, 0xf8000000);
	rtw8189f_iqk_delay(sc);

	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x000000);
	rtw8189f_rf_rmw20(sc, 0xdf, 0xfffff, 0x180);
	*lokp = rtw8189f_rf_read20(sc, RTW8189F_RF_LOK);

	eac = rtw8189f_mac_read_4(sc, RTW8189F_BB_RXPW_AFTER_IQK_A2);
	e94 = rtw8189f_mac_read_4(sc, RTW8189F_BB_TXPW_BEFORE_IQK_A);
	e9c = rtw8189f_mac_read_4(sc, RTW8189F_BB_TXPW_AFTER_IQK_A);
	DNPRINTF(sc, RTW8189F_DBG_INIT,
	    "iqk tx: eac %08x e94 %08x e9c %08x lok %05x\n",
	    eac, e94, e9c, *lokp);

	if (!(eac & __BIT(28)) &&
	    ((e94 & 0x03ff0000) >> 16) != 0x142 &&
	    ((e9c & 0x03ff0000) >> 16) != 0x42)
		return 1;
	return 0;
}

/*
 * RX IQK, two phases (vendor phy_path_a_rx_iqk_8188f).  Returns bit0 set
 * when the TX image check passed and bit1 when RX IQK converged.
 */
static uint8_t
rtw8189f_iqk_rx(struct rtw8189f_softc *sc, uint32_t lok)
{
	uint32_t eac, e94, e9c, ea4, u4tmp;

	/* Phase 1: measure the TX image with the LUT table. */
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x000000);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_WE_LUT, __BIT(19), 1);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_RCK_OS, 0xfffff, 0x30000);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_TXPA_G1, 0xfffff, 0x0000f);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_TXPA_G2, 0xfffff, 0xf1173);
	rtw8189f_rf_rmw20(sc, 0xdf, 0xfffff, 0x980);
	rtw8189f_rf_rmw20(sc, 0x56, 0xfffff, 0x5102a);

	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x808000);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK, 0xffffffff, 0x01007c00);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK, 0xffffffff, 0x01004800);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK_TONE_A, 0xffffffff, 0x10008c1c);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_TONE_A, 0xffffffff, 0x30008c1c);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK_PI_A, 0xffffffff, 0x82160fff);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_PI_A, 0xffffffff, 0x28160000);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_RSP, 0xffffffff, 0x00462911);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_PTS, 0xffffffff, 0xf9000000);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_PTS, 0xffffffff, 0xf8000000);
	rtw8189f_iqk_delay(sc);

	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x000000);
	rtw8189f_rf_rmw20(sc, 0xdf, 0xfffff, 0x180);

	eac = rtw8189f_mac_read_4(sc, RTW8189F_BB_RXPW_AFTER_IQK_A2);
	e94 = rtw8189f_mac_read_4(sc, RTW8189F_BB_TXPW_BEFORE_IQK_A);
	e9c = rtw8189f_mac_read_4(sc, RTW8189F_BB_TXPW_AFTER_IQK_A);
	DNPRINTF(sc, RTW8189F_DBG_INIT,
	    "iqk rx1: eac %08x e94 %08x e9c %08x\n", eac, e94, e9c);
	if ((eac & __BIT(28)) ||
	    ((e94 & 0x03ff0000) >> 16) == 0x142 ||
	    ((e9c & 0x03ff0000) >> 16) == 0x42)
		return 0;	/* if TX not OK, ignore RX */

	u4tmp = 0x80007c00 | (e94 & 0x3ff0000) | ((e9c & 0x3ff0000) >> 16);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK, 0xffffffff, u4tmp);

	/* Phase 2: RX IQK with the TX params latched above. */
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x000000);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_WE_LUT, __BIT(19), 1);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_RCK_OS, 0xfffff, 0x30000);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_TXPA_G1, 0xfffff, 0x0000f);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_TXPA_G2, 0xfffff, 0xf7ff2);
	rtw8189f_rf_rmw20(sc, 0xdf, 0xfffff, 0x980);
	rtw8189f_rf_rmw20(sc, 0x56, 0xfffff, 0x51000);

	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x808000);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK, 0xffffffff, 0x01004800);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK_TONE_A, 0xffffffff, 0x30008c1c);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_TONE_A, 0xffffffff, 0x10008c1c);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK_PI_A, 0xffffffff, 0x82160000);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_PI_A, 0xffffffff, 0x281613ff);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_RSP, 0xffffffff, 0x0046a911);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_PTS, 0xffffffff, 0xf9000000);
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK_AGC_PTS, 0xffffffff, 0xf8000000);
	rtw8189f_iqk_delay(sc);

	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x000000);
	rtw8189f_rf_rmw20(sc, 0xdf, 0xfffff, 0x180);
	rtw8189f_rf_rmw20(sc, RTW8189F_RF_LOK, 0xfffff, lok);

	eac = rtw8189f_mac_read_4(sc, RTW8189F_BB_RXPW_AFTER_IQK_A2);
	ea4 = rtw8189f_mac_read_4(sc, RTW8189F_BB_RXPW_BEFORE_IQK_A2);
	DNPRINTF(sc, RTW8189F_DBG_INIT,
	    "iqk rx2: eac %08x ea4 %08x\n", eac, ea4);
	if (!(eac & __BIT(27)) &&
	    ((ea4 & 0x03ff0000) >> 16) != 0x132 &&
	    ((eac & 0x03ff0000) >> 16) != 0x36)
		return 0x03;
	return 0x01;
}

/*
 * Convergence check across calibration passes (vendor
 * phy_simularity_compare_8188f, bound 8; path B entries stay zero on
 * this 1T1R part and always compare equal).
 */
static bool
rtw8189f_iqk_simular(struct rtw8189f_softc *sc, int32_t result[][8],
    uint8_t c1, uint8_t c2)
{
	uint32_t i, j, simularity_bit_map = 0;
	uint8_t final_candidate[2] = { 0xff, 0xff };
	bool is_result = true;
	int32_t tmp1, tmp2, diff;

	for (i = 0; i < 8; i++) {
		if (i == 1 || i == 3 || i == 5 || i == 7) {
			tmp1 = (result[c1][i] & 0x200) ?
			    result[c1][i] | 0xfffffc00 : result[c1][i];
			tmp2 = (result[c2][i] & 0x200) ?
			    result[c2][i] | 0xfffffc00 : result[c2][i];
		} else {
			tmp1 = result[c1][i];
			tmp2 = result[c2][i];
		}
		diff = (tmp1 > tmp2) ? tmp1 - tmp2 : tmp2 - tmp1;
		if (diff > RTW8189F_IQK_MAX_TOLERANCE) {
			if ((i == 2 || i == 6) && simularity_bit_map == 0) {
				if (result[c1][i] + result[c1][i + 1] == 0)
					final_candidate[i / 4] = c2;
				else if (result[c2][i] + result[c2][i + 1] == 0)
					final_candidate[i / 4] = c1;
				else
					simularity_bit_map |= 1 << i;
			} else
				simularity_bit_map |= 1 << i;
		}
	}

	if (simularity_bit_map == 0) {
		for (i = 0; i < 2; i++) {
			if (final_candidate[i] != 0xff) {
				for (j = i * 4; j < (i + 1) * 4 - 2; j++)
					result[3][j] =
					    result[final_candidate[i]][j];
				is_result = false;
			}
		}
		return is_result;
	}

	if (!(simularity_bit_map & 0x03))
		for (i = 0; i < 2; i++)
			result[3][i] = result[c1][i];
	if (!(simularity_bit_map & 0x0c))
		for (i = 2; i < 4; i++)
			result[3][i] = result[c1][i];
	return false;
}

/* Apply the winning pass (vendor _phy_path_a_fill_iqk_matrix8188f). */
static void
rtw8189f_iqk_fill_matrix(struct rtw8189f_softc *sc, int32_t result[][8],
    uint8_t fc)
{
	uint32_t oldval_0, x, tx0_a;
	int32_t y, tx0_c;

	if (fc == 0xff)
		return;

	oldval_0 = (rtw8189f_mac_read_4(sc, RTW8189F_BB_XA_TXIQK) >> 22) &
	    0x3ff;

	x = (uint32_t)result[fc][0];
	if (x & 0x200)
		x |= 0xfffffc00;
	tx0_a = (x * oldval_0) >> 8;
	DNPRINTF(sc, RTW8189F_DBG_INIT, "iqk fill: x %u tx0_a %x old %x\n",
	    x, tx0_a, oldval_0);
	rtw8189f_bb_set(sc, RTW8189F_BB_XA_TXIQK, 0x3ff, tx0_a);
	rtw8189f_bb_set(sc, RTW8189F_BB_ECCA_THRESHOLD, __BIT(31),
	    ((x * oldval_0 >> 7) & 0x1));

	y = result[fc][1];
	if (y & 0x200)
		y |= 0xfffffc00;
	tx0_c = (int32_t)((uint32_t)y * oldval_0) >> 8;
	rtw8189f_bb_set(sc, RTW8189F_BB_XC_TXAFE, 0xf0000000,
	    (uint32_t)(tx0_c & 0x3c0) >> 6);
	rtw8189f_bb_set(sc, RTW8189F_BB_XA_TXIQK, 0x003f0000, tx0_c & 0x3f);
	rtw8189f_bb_set(sc, RTW8189F_BB_ECCA_THRESHOLD, __BIT(29),
	    (((uint32_t)y * oldval_0 >> 7) & 0x1));

	if (result[fc][2] == 0) {
		/* TX-only calibration: leave the RX IQC untouched. */
		DNPRINTF(sc, RTW8189F_DBG_INIT, "iqk fill: tx only\n");
		return;
	}

	rtw8189f_bb_set(sc, RTW8189F_BB_XA_RXIQK, 0x3ff, result[fc][2]);
	rtw8189f_bb_set(sc, RTW8189F_BB_XA_RXIQK, 0xfc00,
	    result[fc][3] & 0x3f);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_EXT_ANT, 0xf0000000,
	    (result[fc][3] >> 6) & 0xf);
}

/* One calibration pass (vendor _phy_iq_calibrate_8188f, retry_count 2). */
static void
rtw8189f_iqk_pass(struct rtw8189f_softc *sc, int32_t result[][8], uint8_t t,
    uint32_t *adda_bk, uint32_t *mac_bk, uint32_t *bb_bk)
{
	uint8_t retry, path_aok = 0, c50;
	uint32_t lok = sc->sc_lok;

	c50 = rtw8189f_bb_get(sc, RTW8189F_BB_RXIQK_INITGAIN, 0xff);

	if (t == 0)
		rtw8189f_iqk_save(sc, adda_bk, mac_bk, bb_bk);
	rtw8189f_iqk_adda_on(sc);

	rtw8189f_bb_set(sc, RTW8189F_BB_TRX_PATH_ENABLE, 0xffffffff,
	    0x03a05600);
	rtw8189f_bb_set(sc, RTW8189F_BB_TR_MUX_PAR, 0xffffffff, 0x000800e4);
	rtw8189f_bb_set(sc, RTW8189F_BB_XCD_RF_INTERFACE_SW, 0xffffffff,
	    0x25204000);

	/* MAC settings for calibration (only 0x520[23:16] in vendor). */
	rtw8189f_bb_set(sc, RTW8189F_BB_CAL_LIFETIME, 0x00ff0000, 0xff);

	/* IQ calibration setting. */
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x808000);
	rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK, 0xffffffff, 0x01007c00);
	rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK, 0xffffffff, 0x01004800);

	for (retry = 0; retry < 2; retry++) {
		path_aok = rtw8189f_iqk_tx(sc, &lok);
		if (path_aok == 0x01) {
			rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00,
			    0x000000);
			sc->sc_lok = lok;
			DNPRINTF(sc, RTW8189F_DBG_INIT,
			    "path a tx iqk ok (pass %u)\n", t);
			result[t][0] =
			    (rtw8189f_mac_read_4(sc,
			    RTW8189F_BB_TXPW_BEFORE_IQK_A) & 0x3ff0000) >> 16;
			result[t][1] =
			    (rtw8189f_mac_read_4(sc,
			    RTW8189F_BB_TXPW_AFTER_IQK_A) & 0x3ff0000) >> 16;
			break;
		}
	}

	for (retry = 0; retry < 2; retry++) {
		path_aok = rtw8189f_iqk_rx(sc, sc->sc_lok);
		if (path_aok == 0x03) {
			DNPRINTF(sc, RTW8189F_DBG_INIT,
			    "path a rx iqk ok (pass %u)\n", t);
			result[t][2] =
			    (rtw8189f_mac_read_4(sc,
			    RTW8189F_BB_RXPW_BEFORE_IQK_A2) & 0x3ff0000) >> 16;
			result[t][3] =
			    (rtw8189f_mac_read_4(sc,
			    RTW8189F_BB_RXPW_AFTER_IQK_A2) & 0x3ff0000) >> 16;
			break;
		}
		DNPRINTF(sc, RTW8189F_DBG_INIT,
		    "path a rx iqk fail (pass %u try %u)\n", t, retry);
	}
	if (path_aok == 0)
		DNPRINTF(sc, RTW8189F_DBG_INIT, "path a iqk failed\n");

	/* Back to BB mode; all-but-the-first pass also restores context. */
	rtw8189f_bb_set(sc, RTW8189F_BB_IQK, 0xffffff00, 0x000000);
	if (t != 0) {
		rtw8189f_iqk_reload(sc, adda_bk, mac_bk, bb_bk);
		/* Restore the RX initial gain (0x50 glitch trick). */
		rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_INITGAIN, 0xff, 0x50);
		rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_INITGAIN, 0xff, c50);
		/* 0xe30 IQC default values. */
		rtw8189f_bb_set(sc, RTW8189F_BB_TXIQK_TONE_A, 0xffffffff,
		    0x01008c00);
		rtw8189f_bb_set(sc, RTW8189F_BB_RXIQK_TONE_A, 0xffffffff,
		    0x01008c00);
	}
}

/*
 * Full calibration (vendor phy_iq_calibrate_8188f): three passes, pick
 * the convergent result, apply the IQK correction matrix.  Runs once at
 * the first channel switch, matching the vendor's neediqk_24g hook.
 */
static void
rtw8189f_iqk(struct rtw8189f_softc *sc)
{
	int32_t result[4][8];
	uint32_t adda_bk[16], mac_bk[4], bb_bk[9];
	int32_t sum;
	uint8_t i, j, fc = 0xff;

	memset(result, 0, sizeof(result));
	memset(adda_bk, 0, sizeof(adda_bk));
	memset(mac_bk, 0, sizeof(mac_bk));
	memset(bb_bk, 0, sizeof(bb_bk));

	for (i = 0; i < 3; i++) {
		rtw8189f_iqk_pass(sc, result, i, adda_bk, mac_bk, bb_bk);
		if (i == 1) {
			if (rtw8189f_iqk_simular(sc, result, 0, 1)) {
				fc = 0;
				break;
			}
		}
		if (i == 2) {
			if (rtw8189f_iqk_simular(sc, result, 0, 2)) {
				fc = 0;
				break;
			}
			if (rtw8189f_iqk_simular(sc, result, 1, 2)) {
				fc = 1;
			} else {
				sum = 0;
				for (j = 0; j < 8; j++)
					sum += result[3][j];
				fc = (sum != 0) ? 3 : 0xff;
			}
		}
	}

	DNPRINTF(sc, RTW8189F_DBG_INIT, "iqk: final candidate %u\n", fc);
	rtw8189f_iqk_fill_matrix(sc, result, fc);
	sc->sc_iqk_done = true;
	DNPRINTF(sc, RTW8189F_DBG_INIT,
	    "iqk done: c80 %08x c94 %08x c14 %08x (gt 393d00e2/f0000000/"
	    "40000d00)\n",
	    rtw8189f_mac_read_4(sc, RTW8189F_BB_XA_TXIQK),
	    rtw8189f_mac_read_4(sc, RTW8189F_BB_XC_TXAFE),
	    rtw8189f_mac_read_4(sc, RTW8189F_BB_XA_RXIQK));
}

void
rtw8189f_set_channel(struct rtw8189f_softc *sc, unsigned chan)
{
	uint32_t want;

	if (chan < 1 || chan > 14)
		return;

	want = (sc->sc_rf18 & ~(0xffU | __BIT(15))) | chan;
	rtw8189f_rf_write20(sc, RTW8189F_RF_CHNLBW, want);
	/* Vendor reprograms TX power on every channel switch
	 * (PHY_SetSwChnlBWMode8188F -> set_tx_power_level). */
	rtw8189f_set_txpower(sc, chan);
	/* Vendor runs IQK on the first channel switch after init
	 * (neediqk_24g seeded right after the init LCK).  Without it the
	 * TX constellation is never corrected: the AP cannot demodulate
	 * our frames and never ACKs (M4 RETRY_OVER signature). */
	if (!sc->sc_iqk_done)
		rtw8189f_iqk(sc);
	/* DBG_RX: per-channel verification print, would flood at DBG_INIT
	 * during scan. */
	DNPRINTF(sc, RTW8189F_DBG_RX,
	    "setchan %u rf18 want 0x%05x got 0x%05x\n", chan, want,
	    rtw8189f_rf_read20(sc, RTW8189F_RF_CHNLBW));
}

/*
 * Scan window RX filter: the CBSSID bits gate beacons/probe responses on
 * a BSSID match against REG_BSSID, which is not programmed during a
 * scan -- exactly the rtw89 lane's A_BCN_CHK_EN trap.  AAP stays on
 * permanently (8821CU lesson: AP unicast responses need it).
 */
void
rtw8189f_scan_rx_fltr(struct rtw8189f_softc *sc, bool widen)
{
	uint32_t rcr = sc->sc_rcr;
	const uint32_t scan_bits = RTW8189F_RCR_CBSSID_BCN | RTW8189F_RCR_CBSSID_DATA;

	if (widen)
		rcr &= ~scan_bits;
	rtw8189f_mac_write_4(sc, RTW8189F_REG_RCR, rcr);
}

void
rtw8189f_set_bssid(struct rtw8189f_softc *sc, const uint8_t *bssid)
{
	rtw8189f_mac_write_4(sc, RTW8189F_REG_BSSID,
	    (uint32_t)bssid[0] | ((uint32_t)bssid[1] << 8) |
	    ((uint32_t)bssid[2] << 16) | ((uint32_t)bssid[3] << 24));
	rtw8189f_mac_write_2(sc, RTW8189F_REG_BSSID + 4,
	    (uint16_t)(bssid[4] | (bssid[5] << 8)));
}

/* ------------------------------------------------------------------ */
/* TX                                                                  */
/* ------------------------------------------------------------------ */

static uint16_t
rtw8189f_txdesc_chksum(uint8_t *desc)
{
	uint16_t sum = 0;
	int i;

	desc[28] = 0;
	desc[29] = 0;
	for (i = 0; i < 16; i++)
		sum ^= le16dec(desc + 2 * i);
	return sum;
}

#define RTW8189F_TX_QUEUE_IDX_HI	0

void
rtw8189f_tx_frame(struct rtw8189f_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = ic->ic_ifp;
	struct ieee80211_node *ni;
	struct ieee80211_frame *wh;
	uint8_t *buf = sc->sc_txbuf;
	uint32_t len, pages, free_hi, free_pub, rptseq;
	unsigned rate;
	int tries;

	rptseq = (uint32_t)-1;

	ni = M_GETCTX(m, struct ieee80211_node *);
	if (ni == NULL) {
		m_freem(m);
		return;
	}

	wh = mtod(m, struct ieee80211_frame *);
	rate = (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT
	    ? RTW8189F_RATE_1M : RTW8189F_RATE_6M;

	len = (uint32_t)m->m_pkthdr.len;
	if (len + RTW8189F_TXDESC_SIZE > RTW8189F_TXBUFSZ) {
		if_statinc(ifp, if_oerrors);
		goto out;
	}

	memset(buf, 0, RTW8189F_TXDESC_SIZE);
	m_copydata(m, 0, len, buf + RTW8189F_TXDESC_SIZE);

	le32enc(buf + 0, (len & RTW8189F_TXDW0_PKTLEN_M) |
	    (RTW8189F_TXDESC_SIZE << RTW8189F_TXDW0_OFFSET_S));
	le32enc(buf + 4, RTW8189F_TXDESC_QSEL_MGNT << RTW8189F_TXDW1_QSEL_S);
	le32enc(buf + 12, RTW8189F_TXDW3_USE_RATE);
	le32enc(buf + 16, rate & RTW8189F_TXDW4_RATE_M);
	if ((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT) {
		/* Vendor MGNT_FRAMETAG fill: rate table for retries and a
		 * bounded retry count.  Unicast mgmt also asks the firmware
		 * for a CCX TX report (C2H 0x03); the echoed SW_DEFINE says
		 * which frame a report belongs to.  Broadcast/multicast gets
		 * the BMC flag instead. */
		le32enc(buf + 4, le32dec(buf + 4) |
		    (RTW8189F_RATEID_G << RTW8189F_TXDW1_RATEID_S));
		le32enc(buf + 12, le32dec(buf + 12) |
		    RTW8189F_TXDW3_RETRY_LIMIT_EN |
		    (6u << RTW8189F_TXDW3_DATA_RETRY_LIMIT_S));
		if ((wh->i_addr1[0] & 0x01) == 0) {
			rptseq = sc->sc_txrpt_seq++ & RTW8189F_TXDW6_SW_DEFINE_M;
			le32enc(buf + 8, RTW8189F_TXDW2_SPE_RPT);
			le32enc(buf + 24, rptseq);
		} else {
			le32enc(buf + 0, le32dec(buf + 0) | RTW8189F_TXDW0_BMC);
		}
	} else if ((wh->i_addr1[0] & 0x01) == 0) {
		/* Unicast data asks for the same CCX report: the 4-way
		 * EAPOL frames are data-class, and RETRY_OVER vs TXOK on
		 * msg 2/4 tells an inaudible frame from a rejected one. */
		rptseq = sc->sc_txrpt_seq++ & RTW8189F_TXDW6_SW_DEFINE_M;
		le32enc(buf + 8, RTW8189F_TXDW2_SPE_RPT);
		le32enc(buf + 24, rptseq);
	}
	le32enc(buf + 32, RTW8189F_TXDW8_HWSEQ_EN);
	le16enc(buf + 28, rtw8189f_txdesc_chksum(buf));

	len = (len + RTW8189F_TXDESC_SIZE + 3) & ~3u;
	pages = (len + 127) / 128;

	/* Wait for HIQ (+public) pages, vendor polling mode. */
	for (tries = 0;; tries++) {
		free_hi = rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_FREE_TXPG + 0);
		free_pub = rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_FREE_TXPG + 6);
		if (free_hi + free_pub >= pages)
			break;
		if (tries >= 100 || sc->sc_dying) {
			DPRINTF(sc, "tx: no free pages (%u+%u < %u)\n",
			    free_hi, free_pub, pages);
			if_statinc(ifp, if_oerrors);
			goto out;
		}
		kpause("rtw8189ft", true, mstohz(50), NULL);
	}

	if (rtw8189f_fifo_write(sc, RTW8189F_WLAN_TX_HIQ_DEVICE_ID, buf, len) != 0) {
		DPRINTF(sc, "tx: fifo write failed\n");
		if_statinc(ifp, if_oerrors);
		goto out;
	}


	if_statinc(ifp, if_opackets);
	sc->sc_tx_frames++;
	DNPRINTF(sc, RTW8189F_DBG_TX, "tx %s len %u rate %u ch %u rpt %u\n",
	    (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT
	    ? "mgmt" : "data", len, rate,
	    ieee80211_chan2ieee(&sc->sc_ic, sc->sc_ic.ic_curchan),
	    rptseq);
out:
	ieee80211_free_node(ni);
	m_freem(m);
}

/* ------------------------------------------------------------------ */
/* RX                                                                  */
/* ------------------------------------------------------------------ */

static uint16_t
rtw8189f_rx_req_len(struct rtw8189f_softc *sc)
{
	uint16_t len;

	len = (uint16_t)rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_RX0_REQ_LEN);
	len |= (uint16_t)rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_RX0_REQ_LEN + 1) << 8;
	/* Vendor carry fix for length multiples of 256. */
	if ((len % 256) == 0)
		len += rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_RX0_REQ_LEN);
	return len;
}

void
rtw8189f_rx_drain(struct rtw8189f_softc *sc)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = ic->ic_ifp;
	int s;
	unsigned pkt;

	for (pkt = 0; pkt < 64; pkt++) {
		uint8_t *buf = sc->sc_rxbuf;
		uint16_t rxlen;
		uint32_t off;
		uint16_t reqlen;

		reqlen = rtw8189f_rx_req_len(sc);
		if (reqlen == 0)
			break;
		if (reqlen > RTW8189F_RXBUFSZ) {
			/* Desynchronised FIFO report: flush the reported
			 * burst in bounded chunks rather than dying. */
			unsigned chunk;

			sc->sc_rx_errors++;
			DNPRINTF(sc, RTW8189F_DBG_RX,
			    "rx: oversized request %u, flushing\n", reqlen);
			for (chunk = 0; chunk < 8; chunk++)
				rtw8189f_fifo_read(sc, sc->sc_rxbuf,
				    RTW8189F_RXBUFSZ);
			continue;
		}
		rxlen = (reqlen + 3) & ~3u;

		if (rtw8189f_fifo_read(sc, buf, rxlen) != 0) {
			sc->sc_rx_errors++;
			return;
		}

		off = 0;
		while (off + 24 <= rxlen) {
			uint32_t desc_off = off;
			uint32_t d0 = le32dec(buf + off);
			uint32_t d2 = le32dec(buf + off + 8);
			uint32_t pkt_len = d0 & 0x3fff;
			uint32_t drvinfo = ((d0 >> 16) & 0xf) << 3;
			uint32_t shift = (d0 >> 24) & 0x3;
			bool physt = (d0 & __BIT(26)) != 0;
			uint32_t total, frame_off;
			struct mbuf *m;
			struct ieee80211_node *ni;
			uint8_t fc0;
			int rssi;

			total = 24 + drvinfo + shift + pkt_len;
			frame_off = off + 24 + drvinfo + shift;
			off += (total + 7) & ~7u;

			/* Validate the complete record before inspecting any payload. */
			if (total > rxlen - desc_off || pkt_len == 0) {
				sc->sc_rx_errors++;
				break;
			}

			if (d2 & __BIT(28)) {		/* C2H event */
				if (pkt_len < 3 ||
				    (buf[frame_off] == 0x03 && pkt_len < 9)) {
					sc->sc_rx_errors++;
					continue;
				}
				char ph[4 * 10 + 1] = { 0 };
				uint8_t id = buf[frame_off];
				uint8_t cseq = buf[frame_off + 1];
				uint8_t b0 = buf[frame_off + 2];
				unsigned i, n;

				/* CCX TX report (0x03): payload byte0 bit7 =
				 * retry over, bit6 = lifetime over, byte6
				 * echoes TXDESC SW_DEFINE (CCX_FwC2HTxRpt). */
				n = pkt_len >= 2 && pkt_len - 2 < sizeof(ph) / 3
				    ? pkt_len - 2 : 10;
				if (n > 10)
					n = 10;
				for (i = 0; i < n; i++)
					snprintf(ph + 3 * i, sizeof(ph) - 3 * i,
					    "%02x ", buf[frame_off + 2 + i]);
				DNPRINTF(sc, RTW8189F_DBG_INIT,
				    "c2h id %02x seq %u len %u b0 %02x "
				    "rpt %u %s%s\n", id, cseq, pkt_len, b0,
				    pkt_len >= 9 ? buf[frame_off + 8] : 0, ph,
				    id == 0x03 ?
				    ((b0 & 0x80) ? " RETRY_OVER" :
				     (b0 & 0x40) ? " LIFETIME_OVER" : " TXOK") :
				    "");
				continue;
			}

			if ((d0 & __BIT(14)) || pkt_len == 0 ||
			    frame_off + pkt_len > rxlen ||
			    pkt_len < sizeof(struct ieee80211_frame_min) ||
			    pkt_len > MCLBYTES - 16) {
				sc->sc_rx_errors++;
				DNPRINTF(sc, RTW8189F_DBG_RX,
				    "rx: bad pkt d0 %08x len %u\n", d0, pkt_len);
				continue;
			}

			/* PHY status dword0 carries the PWDB report; the
			 * net80211 scan cache only needs monotonic units. */
			rssi = 30;
			if (physt) {
				/* PHY status byte 0 carries the PWDB-style
				 * signal report; byte 2 is the (0-based)
				 * receive channel. */
				rssi = buf[desc_off + 24] & 0x7f;
				if (rssi == 0)
					rssi = 1;
			}

			m = m_gethdr(M_DONTWAIT, MT_DATA);
			if (m == NULL) {
				sc->sc_rx_errors++;
				continue;
			}
			if (pkt_len > MHLEN)
				MCLGET(m, M_DONTWAIT);
			if (pkt_len > MHLEN && !(m->m_flags & M_EXT)) {
				m_freem(m);
				sc->sc_rx_errors++;
				continue;
			}
			m_set_rcvif(m, ifp);
			memcpy(mtod(m, void *), buf + frame_off, pkt_len);
			m->m_pkthdr.len = m->m_len = pkt_len;

			fc0 = *(uint8_t *)(buf + frame_off);
			/* Unicast-to-us discriminator: the M4 branch
			 * criterion.  If this counter stays zero while AUTH
			 * reports RETRY_OVER, the AP's replies are being
			 * dropped on the RX side rather than never sent. */
			if (memcmp(buf + frame_off + 4, sc->sc_mac_addr,
			    IEEE80211_ADDR_LEN) == 0)
				sc->sc_ucast_rx++;
			if ((fc0 & IEEE80211_FC0_TYPE_MASK) ==
			    IEEE80211_FC0_TYPE_MGT &&
			    ((fc0 & IEEE80211_FC0_SUBTYPE_MASK) == IEEE80211_FC0_SUBTYPE_AUTH ||
			    (fc0 & IEEE80211_FC0_SUBTYPE_MASK) == IEEE80211_FC0_SUBTYPE_ASSOC_RESP)) {
				/* Non-beacon mgmt (auth/assoc rsp, deauth,
				 * EAPOL-adjacent): reaching this print means
				 * the FIFO delivered it, so a missing auth
				 * response is a TX-audibility problem, not an
				 * RX filter drop. */
				DNPRINTF(sc, RTW8189F_DBG_INIT,
				    "mgmtrx fc %02x pwdb %u cur %u\n", fc0,
				    buf[desc_off + 24],
				    ieee80211_chan2ieee(ic, ic->ic_curchan));
			}
			if ((fc0 & IEEE80211_FC0_TYPE_MASK) ==
			    IEEE80211_FC0_TYPE_MGT &&
			    ((fc0 & IEEE80211_FC0_SUBTYPE_MASK) ==
			    IEEE80211_FC0_SUBTYPE_BEACON ||
			    (fc0 & IEEE80211_FC0_SUBTYPE_MASK) ==
			    IEEE80211_FC0_SUBTYPE_PROBE_RESP)) {
				const uint8_t *ie, *eie;
				unsigned ds = 0;

				sc->sc_rx_beacons++;
				/* Beacon/probe-rsp fixed part after the
				 * 802.11 header is 8+2+2 bytes; walk the
				 * IEs for the DS parameter set (id 3) and
				 * replicate net80211's channel check. */
				ie = buf + frame_off + 36;
				eie = buf + frame_off + pkt_len;
				for (; ie + 2 <= eie; ie += 2 + ie[1]) {
					if (ie[0] == 3 && ie + 3 <= eie) {
						ds = ie[2];
						break;
					}
				}
				DNPRINTF(sc, RTW8189F_DBG_RX,
				    "bcn #%u fc %02x ds %u cur %u pwdb %u "
				    "hwc %u fscan %d\n",
				    sc->sc_rx_beacons, fc0, ds,
				    ieee80211_chan2ieee(ic, ic->ic_curchan),
				    buf[desc_off + 24],
				    buf[desc_off + 26] & 0xf,
				    (ic->ic_flags & IEEE80211_F_SCAN) ? 1 : 0);
			}

			s = splnet();
			ni = ieee80211_find_rxnode(ic,
			    mtod(m, struct ieee80211_frame_min *));
			if (ni != NULL) {
				ieee80211_input(ic, m, ni, rssi, 0);
				ieee80211_free_node(ni);
			} else
				m_freem(m);
			splx(s);

			sc->sc_rx_frames++;
		}
	}
}
