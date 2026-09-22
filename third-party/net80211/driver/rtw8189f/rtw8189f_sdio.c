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
 * SDIO transport for the RTL8189FTV.
 *
 * MAC/BB/RF registers live in the "WLAN I/O register" area (DeviceID 8),
 * the SDIO-local registers in DeviceID 0.  One-byte accesses always go
 * out as CMD52 (that is also the only reliable form before the MAC is
 * powered); 2/4-byte accesses use CMD53 incremental transfers, which is
 * what the vendor driver does and what the power-on self test relies on
 * (it compares CMD52 byte reads with a CMD53 dword read of REG_CR).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <dev/sdmmc/sdmmcvar.h>
#include <dev/sdmmc/sdmmcchip.h>

#include "rtw8189fvar.h"

static uint32_t
rtw8189f_sdiolocal_addr(uint16_t reg)
{
	return (RTW8189F_SDIO_LOCAL_DEVICE_ID << 13) | (reg & RTW8189F_SDIO_LOCAL_MSK);
}

static uint32_t
rtw8189f_ioreg_addr(uint32_t addr)
{
	return (RTW8189F_WLAN_IOREG_DEVICE_ID << 13) | (addr & RTW8189F_WLAN_IOREG_MSK);
}

uint8_t
rtw8189f_sdiolocal_read_1(struct rtw8189f_softc *sc, uint16_t reg)
{
	return sdmmc_io_read_1(sc->sc_sf, rtw8189f_sdiolocal_addr(reg));
}

void
rtw8189f_sdiolocal_write_1(struct rtw8189f_softc *sc, uint16_t reg,
    uint8_t val)
{
	sdmmc_io_write_1(sc->sc_sf, rtw8189f_sdiolocal_addr(reg), val);
}

uint8_t
rtw8189f_mac_read_1(struct rtw8189f_softc *sc, uint32_t addr)
{
	return sdmmc_io_read_1(sc->sc_sf, rtw8189f_ioreg_addr(addr));
}

uint16_t
rtw8189f_mac_read_2(struct rtw8189f_softc *sc, uint32_t addr)
{
	/* sdmmc_io_read_2() issues a CMD53 incremental read. */
	return sdmmc_io_read_2(sc->sc_sf, rtw8189f_ioreg_addr(addr));
}

uint32_t
rtw8189f_mac_read_4(struct rtw8189f_softc *sc, uint32_t addr)
{
	return sdmmc_io_read_4(sc->sc_sf, rtw8189f_ioreg_addr(addr));
}

void
rtw8189f_mac_write_1(struct rtw8189f_softc *sc, uint32_t addr, uint8_t val)
{
	sdmmc_io_write_1(sc->sc_sf, rtw8189f_ioreg_addr(addr), val);
}

void
rtw8189f_mac_write_2(struct rtw8189f_softc *sc, uint32_t addr, uint16_t val)
{
	sdmmc_io_write_2(sc->sc_sf, rtw8189f_ioreg_addr(addr), val);
}

void
rtw8189f_mac_write_4(struct rtw8189f_softc *sc, uint32_t addr, uint32_t val)
{
	sdmmc_io_write_4(sc->sc_sf, rtw8189f_ioreg_addr(addr), val);
}

/*
 * FIFO (bulk) transfers.
 *
 * TX: the SDIO FIFO address is not a window -- its low 13 bits carry the
 * payload length in DWORDs of the same command (vendor sdio_ops.c
 * HalSdioGetCmdAddr8188FSdio: (DevID << 13) | (rnd4(len) >> 2)).  All
 * frames therefore go through a 4-byte aligned bounce buffer: sdmmc
 * block-mode CMD53 requires word-aligned buffers, and mbuf payloads are
 * not.
 *
 * RX: reads drain the RX0 FIFO at (7 << 13) | (seq & 0x3); the low two
 * bits are a wrapping sequence number the card expects to advance per
 * read.
 */

int
rtw8189f_fifo_write(struct rtw8189f_softc *sc, unsigned devid,
    const void *buf, size_t len)
{
	uint32_t addr;
	size_t wirelen = len > 512 ? roundup(len, 512) : roundup(len, 4);


	if (wirelen > RTW8189F_TXBUFSZ || (len & 3) != 0)
		return EINVAL;
	memset((uint8_t *)__UNCONST(buf) + len, 0, wirelen - len);

	/* Round to 4 bytes: the length encoding is in dwords. */
	addr = (devid << 13) | ((len & ~3) >> 2);
	KASSERT(((vaddr_t)buf & 3) == 0);
	return sdmmc_io_write_region_1(sc->sc_sf, addr,
	    __UNCONST(buf), (int)wirelen);
}

int
rtw8189f_fifo_read(struct rtw8189f_softc *sc, void *buf, size_t len)
{
	uint32_t addr;
	size_t wirelen = len > 512 ? roundup(len, 512) : roundup(len, 4);

	if (wirelen > RTW8189F_RXBUFSZ)
		return EINVAL;

	addr = (RTW8189F_WLAN_RX0FF_DEVICE_ID << 13) |
	    (sc->sc_rx_fifo_cnt++ & RTW8189F_WLAN_RX0FF_MSK);
	return sdmmc_io_read_region_1(sc->sc_sf, addr,
	    (u_char *)buf, (int)wirelen);
}
