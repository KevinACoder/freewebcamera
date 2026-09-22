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

#ifndef _DEV_SDMMC_RTW8189FVAR_H_
#define _DEV_SDMMC_RTW8189FVAR_H_

#include <sys/param.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/kthread.h>
#include <sys/callout.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_proto.h>

#include <dev/sdmmc/sdmmcvar.h>

#include "rtw8189f_reg.h"
#include "opt_rtw8189f.h"

#ifdef RTW8189F_DEBUG
extern int rtw8189f_debug;
#define DPRINTF(sc, fmt, ...) \
	do { \
		if (rtw8189f_debug) \
			device_printf((sc)->sc_dev, fmt, ##__VA_ARGS__); \
	} while (0)
#define DNPRINTF(sc, n, fmt, ...) \
	do { \
		if (rtw8189f_debug & (n)) \
			device_printf((sc)->sc_dev, fmt, ##__VA_ARGS__); \
	} while (0)
#define RTW8189F_DBG_REG		0x0001
#define RTW8189F_DBG_PWR		0x0002
#define RTW8189F_DBG_FW			0x0004
#define RTW8189F_DBG_EFUSE		0x0008
#define RTW8189F_DBG_INIT		0x0010
#define RTW8189F_DBG_TX			0x0020
#define RTW8189F_DBG_RX			0x0040
#else
#define DPRINTF(sc, fmt, ...)		((void)0)
#define DNPRINTF(sc, n, fmt, ...)	((void)0)
#endif

/* Worker work flags (sc_flags, under sc_work_mtx). */
#define RTW8189F_F_NEWSTATE		0x0001
#define RTW8189F_F_TX			0x0002
#define RTW8189F_F_EXIT			0x0004
#define RTW8189F_F_SCANNEXT		0x0008

/* Bounce buffer sizes: RX must hold the largest aggregated FIFO burst,
 * TX one TXDESC (40B) plus the largest 802.11 frame. */
#define RTW8189F_RXBUFSZ		0x2000
#define RTW8189F_TXBUFSZ		0x1000

MBUFQ_HEAD(rtw8189f_txq);

struct rtw8189f_softc {
	device_t		sc_dev;
	struct sdmmc_function	*sc_sf;		/* SDIO function 1 */
	kmutex_t		sc_lock;	/* attach vs detach */

	struct ieee80211com	sc_ic;
	struct ethercom		sc_ec;
#define sc_if			sc_ec.ec_if
	int			(*sc_newstate)(struct ieee80211com *,
				    enum ieee80211_state, int);
	callout_t		sc_scan_to;

	/* Worker thread: owns all sleeping chip/bus work at runtime. */
	kmutex_t		sc_work_mtx;
	kcondvar_t		sc_cv;
	struct rtw8189f_txq	sc_txq;		/* frames waiting for TX */
	uint32_t		sc_flags;
	enum ieee80211_state	sc_nstate;	/* deferred newstate args */
	int			sc_narg;
	lwp_t			*sc_worker;

	int			sc_dying;
	bool			sc_attached;
	bool			sc_mac_on;	/* card enable done */
	bool			sc_fw_ready;
	bool			sc_chip_ready;	/* MAC/BB/RF init done */
	bool			sc_scanning;	/* scan window open */
	bool			sc_iqk_done;	/* one-shot IQK ran */
	uint8_t			sc_last_hmebox;
	uint8_t			sc_mac_addr[IEEE80211_ADDR_LEN];
	bool			sc_mac_valid;

	/* eFuse logical map (parsed from the physical map). */
	uint8_t			sc_efuse_map[RTW8189F_HWSET_MAX_SIZE];

	/* TX power (vendor phy_set_tx_power_level_8188F equivalents; the
	 * per-channel index = base[group] + diff, clamped to 0..63). */
	uint8_t			sc_txpwr_cck_base[6];	/* CCK groups 0-5 */
	uint8_t			sc_txpwr_ofdm_base[5];	/* OFDM/BW40 groups 0-4 */
	int8_t			sc_txpwr_ofdm_diff;	/* OFDM-1T diff */
	int8_t			sc_txpwr_bw20_diff;	/* BW20-1T diff */

	/* Firmware image, loaded once the root filesystem is up. */
	void			*sc_fw;
	size_t			sc_fwsize;

	/* RX state. */
	uint32_t		sc_rx_fifo_cnt;
	uint32_t		sc_rcr;		/* current RCR image */
	uint32_t		sc_rf18;	/* cached RF18 image */
	void			*sc_rxbuf;	/* 4-byte aligned bounce */
	void			*sc_txbuf;

	/* Debug counters (sysctl-visible via ifconfig -v is not wired yet). */
	uint32_t		sc_rx_frames;
	uint32_t		sc_rx_beacons;
	uint32_t		sc_rx_errors;
	uint32_t		sc_tx_frames;
	uint32_t		sc_txrpt_seq;	/* SW_DEFINE for C2H TX reports */
	uint32_t		sc_ucast_rx;	/* frames with addr1 == our MAC */
	uint32_t		sc_lok;		/* IQK LOK result (RF 0x08) */
};

/* rtw8189f_sdio.c */
uint8_t	rtw8189f_sdiolocal_read_1(struct rtw8189f_softc *, uint16_t);
void	rtw8189f_sdiolocal_write_1(struct rtw8189f_softc *, uint16_t, uint8_t);
uint8_t	rtw8189f_mac_read_1(struct rtw8189f_softc *, uint32_t);
uint16_t rtw8189f_mac_read_2(struct rtw8189f_softc *, uint32_t);
uint32_t rtw8189f_mac_read_4(struct rtw8189f_softc *, uint32_t);
void	rtw8189f_mac_write_1(struct rtw8189f_softc *, uint32_t, uint8_t);
void	rtw8189f_mac_write_2(struct rtw8189f_softc *, uint32_t, uint16_t);
void	rtw8189f_mac_write_4(struct rtw8189f_softc *, uint32_t, uint32_t);
int	rtw8189f_fifo_write(struct rtw8189f_softc *, unsigned,
	    const void *, size_t);
int	rtw8189f_fifo_read(struct rtw8189f_softc *, void *, size_t);

/* rtw8189f_chip.c */
int	rtw8189f_power_on(struct rtw8189f_softc *);
int	rtw8189f_power_on_check(struct rtw8189f_softc *);
int	rtw8189f_efuse_read(struct rtw8189f_softc *);
int	rtw8189f_fw_download(struct rtw8189f_softc *);
int	rtw8189f_fw_ready(struct rtw8189f_softc *);
int	rtw8189f_chip_init(struct rtw8189f_softc *);
void	rtw8189f_set_channel(struct rtw8189f_softc *, unsigned);
void	rtw8189f_scan_rx_fltr(struct rtw8189f_softc *, bool);
void	rtw8189f_set_bssid(struct rtw8189f_softc *, const uint8_t *);
void	rtw8189f_tx_frame(struct rtw8189f_softc *, struct mbuf *);
void	rtw8189f_rx_drain(struct rtw8189f_softc *);

#endif /* !_DEV_SDMMC_RTW8189FVAR_H_ */
