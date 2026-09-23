/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by the RK3568 lab (NetBSD/RTL8189FTV SDIO bring-up).
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
 * Realtek RTL8189FTV SDIO 802.11 driver (RTL8188F silicon, 024c:f179).
 *
 * A clean-room port: the chip sequences come from the vendor rtl8189fs
 * Linux driver used as reference only (see rtw8189f_chip.c), the
 * transport talks sdmmc(4) directly (rtw8189f_sdio.c), and this file is
 * the autoconf + net80211 side, following the same pre-HT net80211
 * softmac pattern as if_rtw88.c (11b/g only, software crypto, scan via
 * ic_newstate + callout).
 *
 * The module has no card-detect pin (broken-cd in the board DTS) and its
 * REG_ON is strapped high, so SDIO enumeration happens on its own; this
 * driver matches CIS 0x024c/0xf179 on function 1.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/mbuf.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/socket.h>
#include <sys/callout.h>
#include <sys/mutex.h>
#include <sys/pmf.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_proto.h>

#include <dev/sdmmc/sdmmcvar.h>
#include <dev/firmload.h>

#include "rtw8189fvar.h"

#define RTW8189F_DRVNAME		"if_rtw8189f"
#define RTW8189F_FWNAME			"rtw8189f_fw.bin"
#define RTW8189F_SDIO_VENDOR		0x024c
#define RTW8189F_SDIO_PRODUCT		0xf179

static int	rtw8189f_match(device_t, cfdata_t, void *);
static void	rtw8189f_attach(device_t, device_t, void *);
static int	rtw8189f_detach(device_t, int);
static int	rtw8189f_activate(device_t, enum devact);

CFATTACH_DECL_NEW(rtw8189f, sizeof(struct rtw8189f_softc), rtw8189f_match,
    rtw8189f_attach, rtw8189f_detach, rtw8189f_activate);

static void	rtw8189f_attachhook(device_t);
static int	rtw8189f_load_firmware(struct rtw8189f_softc *);
static int	rtw8189f_init(struct ifnet *);
/* NET80211_PORT(L): SDIO DAT1 card interrupt handler */
static int	rtw8189f_sdio_intr(void *);
static void	rtw8189f_stop(struct ifnet *, int);
static void	rtw8189f_start(struct ifnet *);
static void	rtw8189f_watchdog(struct ifnet *);
static int	rtw8189f_ioctl(struct ifnet *, u_long, void *);
static int	rtw8189f_newstate(struct ieee80211com *, enum ieee80211_state,
			    int);
static void	rtw8189f_newstate_cb(struct rtw8189f_softc *,
			    enum ieee80211_state, int);
static void	rtw8189f_next_scan(void *);
static void	rtw8189f_worker(void *);
static void	rtw8189f_worker_stop(struct rtw8189f_softc *);

static int
rtw8189f_match(device_t parent, cfdata_t match, void *aux)
{
	struct sdmmc_attach_args *saa = aux;
	struct sdmmc_cis *cis;

	/* Not SDIO. */
	if (saa->sf == NULL)
		return 0;

	cis = &saa->sf->sc->sc_fn0->cis;
	if (cis->manufacturer != RTW8189F_SDIO_VENDOR ||
	    cis->product != RTW8189F_SDIO_PRODUCT)
		return 0;

	/* One WiFi function only; fn0 is the common control area. */
	if (saa->sf->number != 1)
		return 0;

	return 1;
}

static void
rtw8189f_attach(device_t parent, device_t self, void *aux)
{
	struct rtw8189f_softc *sc = device_private(self);
	struct sdmmc_attach_args *saa = aux;
	uint8_t himr;

	sc->sc_dev = self;
	sc->sc_sf = saa->sf;

	aprint_naive("\n");
	aprint_normal("\n");

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_work_mtx, MUTEX_DEFAULT, IPL_NET);
	cv_init(&sc->sc_cv, device_xname(self));
	MBUFQ_INIT(&sc->sc_txq);
	callout_init(&sc->sc_scan_to, 0);
	callout_setfunc(&sc->sc_scan_to, rtw8189f_next_scan, sc);

	/* 512-byte blocks for the CMD53 FIFO/register bursts. */
	if (sdmmc_io_set_blocklen(sc->sc_sf, 512) != 0) {
		aprint_error_dev(self, "cannot set block length\n");
		return;
	}
	if (sdmmc_io_function_enable(sc->sc_sf) != 0) {
		aprint_error_dev(self, "cannot enable function 1\n");
		return;
	}

	/* Transport sanity: SDIO-local HIMR must read back something. */
	himr = rtw8189f_sdiolocal_read_1(sc, RTW8189F_SDIO_REG_HIMR);
	aprint_normal_dev(self, "fn1 enabled, 512B blocks, HIMR 0x%02x\n",
	    himr);

	/*
	 * The chip bring-up needs its firmware image from the root
	 * filesystem, which is not mounted yet at attach time.
	 */
	config_mountroot(self, rtw8189f_attachhook);
}

static void
rtw8189f_attachhook(device_t self)
{
	struct rtw8189f_softc *sc = device_private(self);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &sc->sc_if;
	int attempt, error;

	for (attempt = 0; attempt < 40; attempt++) {
		if (sc->sc_dying)
			return;
		error = rtw8189f_load_firmware(sc);
		if (error != ENOENT)
			break;
		/* firmware(9) needs the root filesystem: wait for it. */
		kpause("rtw8189fup", false, mstohz(100), NULL);
	}
	if (error != 0) {
		aprint_error_dev(self, "cannot load firmware (%d)\n", error);
		return;
	}

	/* Card enable + self test + eFuse MAC + firmware download. */
	if (rtw8189f_power_on(sc) != 0) {
		aprint_error_dev(self, "power on failed\n");
		return;
	}
	if (rtw8189f_power_on_check(sc) != 0)
		return;
	if (rtw8189f_efuse_read(sc) != 0) {
		aprint_error_dev(self, "eFuse read failed\n");
		return;
	}
	aprint_normal_dev(self, "Ethernet address %s\n",
	    ether_sprintf(sc->sc_mac_addr));

	/*
	 * Queue geometry BEFORE the firmware boots (deviation, mirrored
	 * to rtl8189fs _InitTransferPageSize ordering): the 8051 samples
	 * the RX page size and the FIFO boundaries when it starts.
	 * Programmed only post-download (the old order) the firmware kept
	 * running with the PBP reset default 0x10 - RX pages of 64 bytes
	 * instead of 128 - and every large received frame was lost on the
	 * air interface while small frames (beacons, ARP, ICMP) passed:
	 * the M11 TCP-downlink stall.  Post-download chip_init still
	 * re-writes these; harmless re-assertion.
	 */
	rtw8189f_mac_write_1(sc, RTW8189F_REG_PBP,
	    RTW8189F_PBP_RX(RTW8189F_PBP_128) | RTW8189F_PBP_TX(RTW8189F_PBP_128));
	rtw8189f_mac_write_1(sc, RTW8189F_REG_RX_DRVINFO_SZ, 4);
	rtw8189f_mac_write_1(sc, RTW8189F_REG_TDECTRL + 1,
	    RTW8189F_TX_PAGE_BOUNDARY);
	rtw8189f_mac_write_2(sc, RTW8189F_REG_TRXFF_BNDY + 2,
	    RTW8189F_RX_DMA_BOUNDARY);

	if (rtw8189f_fw_download(sc) != 0) {
		aprint_error_dev(self, "firmware download failed\n");
		return;
	}

	/* Bounce buffers for the CMD53 FIFO bursts (4-byte aligned). */
	sc->sc_rxbuf = kmem_alloc(RTW8189F_RXBUFSZ, KM_SLEEP);
	sc->sc_txbuf = kmem_alloc(RTW8189F_TXBUFSZ, KM_SLEEP);

	/*
	 * Register with net80211.  The MAC/BB/RF init runs from
	 * rtw8189f_init() on the first ifconfig up.
	 */
	ic->ic_ifp = ifp;
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_state = IEEE80211_S_INIT;
	ic->ic_caps =
	    IEEE80211_C_MONITOR |
	    IEEE80211_C_SHPREAMBLE |
	    IEEE80211_C_SHSLOT |
	    IEEE80211_C_WPA;		/* 802.11i (software crypto) */

	/* 11b/g only: net80211 here has no HT/VHT. */
	ic->ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	ic->ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;

	for (int i = 1; i <= 14; i++) {
		ic->ic_channels[i].ic_freq =
		    ieee80211_ieee2mhz(i, IEEE80211_CHAN_2GHZ);
		ic->ic_channels[i].ic_flags =
		    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ;
	}

	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = rtw8189f_init;
	ifp->if_ioctl = rtw8189f_ioctl;
	ifp->if_start = rtw8189f_start;
	ifp->if_watchdog = rtw8189f_watchdog;
	IFQ_SET_READY(&ifp->if_snd);
	memcpy(ifp->if_xname, device_xname(sc->sc_dev), IFNAMSIZ);

	if_initialize(ifp);
	ieee80211_ifattach(ic);

	sc->sc_newstate = ic->ic_newstate;
	ic->ic_newstate = rtw8189f_newstate;

	ieee80211_media_init(ic, ieee80211_media_change,
	    ieee80211_media_status);

	ifp->if_percpuq = if_percpuq_create(ifp);
	if_register(ifp);

	if_set_sadl(ifp, sc->sc_mac_addr, IEEE80211_ADDR_LEN, false);
	memcpy(ic->ic_myaddr, sc->sc_mac_addr, IEEE80211_ADDR_LEN);

	ieee80211_announce(ic);
	sc->sc_attached = true;

	pmf_device_register(self, NULL, NULL);
}

static int
rtw8189f_load_firmware(struct rtw8189f_softc *sc)
{
	firmware_handle_t fh;
	size_t size;
	int error;

	error = firmware_open(RTW8189F_DRVNAME, RTW8189F_FWNAME, &fh);
	if (error != 0)
		return ENOENT;

	size = firmware_get_size(fh);
	sc->sc_fw = kmem_alloc(size, KM_SLEEP);
	error = firmware_read(fh, 0, sc->sc_fw, size);
	firmware_close(fh);
	if (error != 0) {
		kmem_free(sc->sc_fw, size);
		sc->sc_fw = NULL;
		return error;
	}
	sc->sc_fwsize = size;

	aprint_normal_dev(sc->sc_dev, "loaded %s (%zu bytes)\n",
	    RTW8189F_FWNAME, size);
	return 0;
}

static int
rtw8189f_detach(device_t self, int flags)
{
	struct rtw8189f_softc *sc = device_private(self);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ifnet *ifp = &sc->sc_if;
	int s;

	pmf_device_deregister(self);
	s = splnet();
	sc->sc_dying = 1;
	splx(s);

	rtw8189f_worker_stop(sc);
	s = splnet();
	callout_halt(&sc->sc_scan_to, NULL);

	if (sc->sc_attached) {
		ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
		ieee80211_ifdetach(ic);
		if_detach(ifp);
		sc->sc_attached = false;
	}
	splx(s);

	if (sc->sc_rxbuf != NULL)
		kmem_free(sc->sc_rxbuf, RTW8189F_RXBUFSZ);
	if (sc->sc_txbuf != NULL)
		kmem_free(sc->sc_txbuf, RTW8189F_TXBUFSZ);
	if (sc->sc_fw != NULL)
		kmem_free(sc->sc_fw, sc->sc_fwsize);

	mutex_destroy(&sc->sc_work_mtx);
	cv_destroy(&sc->sc_cv);
	mutex_destroy(&sc->sc_lock);
	callout_destroy(&sc->sc_scan_to);

	return 0;
}

static int
rtw8189f_activate(device_t self, enum devact act)
{
	struct rtw8189f_softc *sc = device_private(self);

	switch (act) {
	case DVACT_DEACTIVATE:
		sc->sc_dying = 1;
		if_deactivate(sc->sc_ic.ic_ifp);
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

/* ------------------------------------------------------------------ */
/* ifnet entry points                                                  */
/* ------------------------------------------------------------------ */

static int
rtw8189f_init(struct ifnet *ifp)
{
	struct rtw8189f_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int error, s;

	if (sc->sc_dying)
		return ENXIO;
	if (!sc->sc_fw_ready)
		return EOPNOTSUPP;

	/* MAC/BB/RF init + LLT/queue/RCR bring-up (sleeping; ioctl ctx). */
	if (!sc->sc_chip_ready) {

		/* A previous worker may still be draining its last loop. */
		{
			int wait;

			for (wait = 0; sc->sc_worker != NULL && wait < 40; wait++)
				kpause("rtw8189fw", false, mstohz(50), NULL);
			if (sc->sc_worker != NULL) {
				sc->sc_chip_ready = false;
				return EBUSY;
			}
		}

		error = rtw8189f_chip_init(sc);
		if (error != 0)
			return error;
		sc->sc_chip_ready = true;

		mutex_enter(&sc->sc_work_mtx);
		sc->sc_flags = 0;
		mutex_exit(&sc->sc_work_mtx);

		error = kthread_create(PRI_NONE, 0, NULL, rtw8189f_worker, sc,
		    &sc->sc_worker, "%s-worker", device_xname(sc->sc_dev));
		if (error != 0) {
			sc->sc_chip_ready = false;
			return error;
		}

		/* NET80211_PORT(L): SDIO DAT1 interrupt mode. Chip-side
		 * RX_REQUEST plus CCCR INT_ENABLE go on with the worker
		 * already running; the 10 ms poll stays armed as the
		 * watchdog (mstohz(RTW8189F_RX_POLL_MS)). */
		sc->sc_ih = sdmmc_intr_establish(sc->sc_sf,
		    rtw8189f_sdio_intr, sc);
		if (sc->sc_ih != NULL) {
			(void) rtw8189f_sdiolocal_write_1(sc,
			    RTW8189F_SDIO_REG_HIMR, RTW8189F_HIMR_RX_REQUEST);
			aprint_normal_dev(sc->sc_dev,
			    "SDIO interrupt mode on\n");
		} else {
			aprint_normal_dev(sc->sc_dev,
			    "SDIO interrupt establish failed, poll-only\n");
		}
	}

	s = splnet();
	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;
	/* 1 Hz tick: ieee80211_watchdog drives the AUTH/ASSOC mgt timers;
	 * without it the state machine never times out nor retries. */
	ifp->if_timer = 1;
	ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
	splx(s);

	return 0;
}

static void
rtw8189f_stop(struct ifnet *ifp, int disable)
{
	struct rtw8189f_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int s;

	s = splnet();
	ifp->if_timer = 0;
	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	callout_stop(&sc->sc_scan_to);
	if (ic->ic_state != IEEE80211_S_INIT)
		ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	splx(s);

	if (sc->sc_ih != NULL) {
		/* kill the chip-side source first, then the host line */
		(void) rtw8189f_sdiolocal_write_1(sc,
		    RTW8189F_SDIO_REG_HIMR, 0);
		sdmmc_intr_disestablish(sc->sc_ih);
		sc->sc_ih = NULL;
	}

	/*
	 * Halt the worker without joining it: the worker can be deep
	 * inside a net80211 callback chain and a join here deadlocks the
	 * ioctl thread (observed on board).  The worker clears
	 * sc_worker itself on exit; init() waits for it before creating
	 * a new one.
	 */
	mutex_enter(&sc->sc_work_mtx);
	sc->sc_flags |= RTW8189F_F_EXIT;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_work_mtx);

	sc->sc_chip_ready = false;
}

static void
rtw8189f_start(struct ifnet *ifp)
{
	struct rtw8189f_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m;
	bool kick = false;
	int s;

	if (!sc->sc_chip_ready || (ifp->if_flags & IFF_RUNNING) == 0) {
		if_statinc(ifp, if_oerrors);
		return;
	}

	s = splnet();
	for (;;) {
		struct ether_header *eh;
		struct ieee80211_node *ni;
		struct ieee80211_frame *wh;

		IF_POLL(&ic->ic_mgtq, m);
		if (m != NULL) {
			IF_DEQUEUE(&ic->ic_mgtq, m);
			/* mgmt mbufs carry their node via M_SETCTX. */
			DNPRINTF(sc, RTW8189F_DBG_TX, "start: mgmt frame queued\n");
			mutex_enter(&sc->sc_work_mtx);
			MBUFQ_ENQUEUE(&sc->sc_txq, m);
			mutex_exit(&sc->sc_work_mtx);
			kick = true;
			continue;
		}

		if (ic->ic_state != IEEE80211_S_RUN)
			break;

		IFQ_POLL(&ifp->if_snd, m);
		if (m == NULL)
			break;
		IFQ_DEQUEUE(&ifp->if_snd, m);

		eh = mtod(m, struct ether_header *);
		ni = ieee80211_find_txnode(ic, eh->ether_dhost);
		if (ni == NULL) {
			if_statinc(ifp, if_oerrors);
			m_freem(m);
			continue;
		}

		if ((m = ieee80211_encap(ic, m, ni)) == NULL) {
			if_statinc(ifp, if_oerrors);
			ieee80211_free_node(ni);
			continue;
		}

		/*
		 * ieee80211_encap() only builds the 802.11 header, sets the
		 * Protected bit and reserves room for the crypto header; a
		 * software-crypto driver has to finish the encapsulation
		 * itself.  Without this the frame leaves with Protected set
		 * but no CCMP header/MIC and the AP discards every data
		 * frame (rtw88 lane lesson).
		 */
		wh = mtod(m, struct ieee80211_frame *);
		if ((wh->i_fc[1] & IEEE80211_FC1_WEP) != 0 &&
		    ieee80211_crypto_encap(ic, ni, m) == NULL) {
			if_statinc(ifp, if_oerrors);
			m_freem(m);
			ieee80211_free_node(ni);
			continue;
		}

		M_SETCTX(m, ni);
		mutex_enter(&sc->sc_work_mtx);
		MBUFQ_ENQUEUE(&sc->sc_txq, m);
		mutex_exit(&sc->sc_work_mtx);
		kick = true;
	}
	splx(s);

	if (kick) {
		mutex_enter(&sc->sc_work_mtx);
		sc->sc_flags |= RTW8189F_F_TX;
		cv_broadcast(&sc->sc_cv);
		mutex_exit(&sc->sc_work_mtx);
	}
}

static void
rtw8189f_watchdog(struct ifnet *ifp)
{
	struct rtw8189f_softc *sc = ifp->if_softc;

	/* Keep ticking while up; stop() clears if_timer. */
	ifp->if_timer = 1;
	ieee80211_watchdog(&sc->sc_ic);
}

static int
rtw8189f_ioctl(struct ifnet *ifp, u_long cmd, void *data)
{
	struct rtw8189f_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = &sc->sc_ic;
	int error = 0;

	/* init/stop perform sleeping bus and worker operations. */
	switch (cmd) {
	case SIOCSIFFLAGS:
		if (sc->sc_dying) {
			error = EIO;
			break;
		}
		if ((error = ifioctl_common(ifp, cmd, data)) != 0)
			break;
		switch (ifp->if_flags & (IFF_UP | IFF_RUNNING)) {
		case IFF_RUNNING:
			rtw8189f_stop(ifp, 1);
			break;
		case IFF_UP:
			error = rtw8189f_init(ifp);
			break;
		default:
			break;
		}
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if ((error = ether_ioctl(ifp, cmd, data)) == ENETRESET)
			error = 0;
		break;

	default:
		error = ieee80211_ioctl(ic, cmd, data);
		break;
	}

	return error;
}

/* ------------------------------------------------------------------ */
/* State machine, scanning and the worker thread                       */
/* ------------------------------------------------------------------ */

/*
 * net80211 here is the pre-FreeBSD-8 stack: scanning is driven by
 * re-entering IEEE80211_S_SCAN through ic_newstate, with the channel
 * net80211 picked in ic_curchan, and each pass probing the channel via
 * ieee80211_probe_curchan() (frames arrive in ic->ic_mgtq).  The state
 * machine runs at splnet while everything below sleeps on SDIO, so the
 * chip work is deferred to the worker thread, exactly as if_rtw88.c
 * does with its async callback.
 */
static int
rtw8189f_newstate(struct ieee80211com *ic, enum ieee80211_state nstate,
    int arg)
{
	struct rtw8189f_softc *sc = ic->ic_ifp->if_softc;

	/*
	 * Only kill the dwell timer when leaving scan.  The first INIT->SCAN
	 * transition re-enters here synchronously (begin_scan -> next_scan ->
	 * new_state(S_SCAN)) before the first dwell has even started; stopping
	 * unconditionally used to cancel that dwell's timer, leaving the RF on
	 * the attach channel (1) for microseconds only.
	 */
	if (nstate != IEEE80211_S_SCAN)
		callout_stop(&sc->sc_scan_to);
	if (sc->sc_worker == NULL || sc->sc_dying || !sc->sc_chip_ready)
		return sc->sc_newstate(ic, nstate, arg);

	mutex_enter(&sc->sc_work_mtx);
	sc->sc_nstate = nstate;
	sc->sc_narg = arg;
	sc->sc_flags |= RTW8189F_F_NEWSTATE;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_work_mtx);

	return 0;
}

/* Runs on the worker; ic->ic_state still holds the previous state. */
static void
rtw8189f_newstate_cb(struct rtw8189f_softc *sc,
    enum ieee80211_state nstate, int arg)
{
	struct ieee80211com *ic = &sc->sc_ic;
	enum ieee80211_state ostate = ic->ic_state;

	/* Real state transitions on the always-on INIT bit; the per-channel
	 * scan hops (1 -> 1) would otherwise flood the console (they stay on
	 * DBG_RX). */
	if (ostate != nstate)
		DNPRINTF(sc, RTW8189F_DBG_INIT, "newstate %d -> %d ch %d "
		    "ucast rx %u\n",
		    ostate, nstate, ieee80211_chan2ieee(ic, ic->ic_curchan),
		    sc->sc_ucast_rx);
	else
		DNPRINTF(sc, RTW8189F_DBG_RX, "newstate %d -> %d ch %d\n",
		    ostate, nstate, ieee80211_chan2ieee(ic, ic->ic_curchan));

	switch (nstate) {
	case IEEE80211_S_SCAN:
		/* One channel per pass; the callout moves to the next. */
		rtw8189f_set_channel(sc,
		    ieee80211_chan2ieee(ic, ic->ic_curchan));
		if (ostate != IEEE80211_S_SCAN && !sc->sc_scanning) {
			sc->sc_scanning = true;
			rtw8189f_scan_rx_fltr(sc, true);
		}
		callout_schedule(&sc->sc_scan_to, hz / 5);
		break;

	case IEEE80211_S_AUTH:
	case IEEE80211_S_ASSOC:
	case IEEE80211_S_RUN:
		if (sc->sc_scanning) {
			sc->sc_scanning = false;
			rtw8189f_scan_rx_fltr(sc, false);
		}
		if (ostate != nstate) {
			rtw8189f_set_channel(sc,
			    ieee80211_chan2ieee(ic, ic->ic_curchan));
			if (ic->ic_opmode == IEEE80211_M_MONITOR) {
				/* Monitor capture: RCR_DEFAULT keeps the CBSSID
				 * bits set and no BSSID is programmed in
				 * monitor mode, so the MAC would drop every
				 * frame.  Widen like a scan window and leave
				 * the filter that way (net80211 routes monitor
				 * frames to bpf). */
				rtw8189f_scan_rx_fltr(sc, true);
				break;
			}
			/* Program the BSSID before AUTH too: the unwidened
			 * filter re-enables BSSID checking and the auth
			 * response must pass it (vendor joins with the
			 * BSSID already written). */
			rtw8189f_set_bssid(sc, ic->ic_bss->ni_bssid);
		}
		break;

	case IEEE80211_S_INIT:
		if (sc->sc_scanning) {
			sc->sc_scanning = false;
			rtw8189f_scan_rx_fltr(sc, false);
		}
		break;
	}

	sc->sc_newstate(ic, nstate, arg);
}

/*
 * Callout context must not sleep: the channel switch has to drain the RX
 * FIFO (CMD53) while ic_curchan still names the old dwell channel, so the
 * callout only wakes the worker and ieee80211_next_scan() runs there.
 */
static void
rtw8189f_next_scan(void *arg)
{
	struct rtw8189f_softc *sc = arg;

	mutex_enter(&sc->sc_work_mtx);
	sc->sc_flags |= RTW8189F_F_SCANNEXT;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_work_mtx);
}

static void
rtw8189f_worker_stop(struct rtw8189f_softc *sc)
{
	int wait;

	mutex_enter(&sc->sc_work_mtx);
	sc->sc_flags |= RTW8189F_F_EXIT;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_work_mtx);

	/* Bounded wait: the worker clears sc_worker itself on exit. */
	for (wait = 0; sc->sc_worker != NULL && wait < 200; wait++)
		kpause("rtw8189fw", false, mstohz(10), NULL);
}

/* NET80211_PORT(L): SDIO card interrupt (DAT1). Runs in host-controller
 * ISR context: latch the RX work flag and hand the worker cv one token -
 * the bus itself is untouchable here. */
static int
rtw8189f_sdio_intr(void *arg)
{
	struct rtw8189f_softc *sc = arg;

	sc->sc_flags |= RTW8189F_F_RX;
	wlan_cv_isr_wake(&sc->sc_cv);
	return 1;
}

static void
rtw8189f_worker(void *arg)
{
	struct rtw8189f_softc *sc = arg;
	struct rtw8189f_txq locq;
	struct mbuf *m;
	struct ieee80211_node *ni;
	uint32_t flags;
	enum ieee80211_state nstate;
	int narg;

	MBUFQ_INIT(&locq);

	while (!sc->sc_dying) {
		mutex_enter(&sc->sc_work_mtx);
		while (!(sc->sc_flags & (RTW8189F_F_NEWSTATE | RTW8189F_F_TX |
		    RTW8189F_F_SCANNEXT | RTW8189F_F_EXIT |
		    RTW8189F_F_RX)) && !sc->sc_dying) {
			/* A timeout is RX work, even without a software event.
			 * PORT: poll quantum is a tunable (opt_rtw8189f.h,
			 * 10 ms here vs NetBSD's 50 ms). */
			if (cv_timedwait(&sc->sc_cv, &sc->sc_work_mtx,
			    mstohz(RTW8189F_RX_POLL_MS)) == EWOULDBLOCK)
				break;
		}
		flags = sc->sc_flags;
		nstate = sc->sc_nstate;
		narg = sc->sc_narg;
		sc->sc_flags = 0;
		/* Steal the TX queue without holding the mutex on the bus. */
		for (;;) {
			MBUFQ_DEQUEUE(&sc->sc_txq, m);
			if (m == NULL)
				break;
			MBUFQ_ENQUEUE(&locq, m);
		}
		mutex_exit(&sc->sc_work_mtx);

		if ((flags & RTW8189F_F_EXIT) || sc->sc_dying)
			break;

		/*
		 * PORT: on NetBSD the ioctl/start contexts hold splnet
		 * while this kthread runs at IPL_NONE; here spl is a no-op,
		 * so the port serializer stands in for the discipline.
		 * Held across the bus and net80211 work (ms-scale CMD53
		 * bursts), dropped around the cv wait above - the same
		 * shape the usbdi shim wraps its completion callbacks in.
		 */
		wlan_port_serializer_lock();

		/*
		 * Drain the FIFO before anything moves ic_curchan: frames
		 * heard on the previous dwell carry that channel in their
		 * DS-param IE and net80211 discards them on mismatch, so
		 * they must be input while the radio is still tuned there.
		 */
		rtw8189f_rx_drain(sc);

		if ((flags & RTW8189F_F_SCANNEXT) &&
		    !(flags & RTW8189F_F_NEWSTATE)) {
			int s = splnet();

			if (sc->sc_ic.ic_state == IEEE80211_S_SCAN)
				ieee80211_next_scan(&sc->sc_ic);
			splx(s);
		}

		if (flags & RTW8189F_F_NEWSTATE)
			rtw8189f_newstate_cb(sc, nstate, narg);

		for (;;) {
			MBUFQ_DEQUEUE(&locq, m);
			if (m == NULL)
				break;
			rtw8189f_tx_frame(sc, m);
		}

		/* NET80211_PORT(L): the DAT1 interrupt makes this drain the
		 * primary RX path (the comment below it predates the round).
		 * The drain doubles as the ack - the chip-side source is
		 * consumed by then, so re-arm the host line; a frame landing
		 * in between simply re-asserts DAT1. */
		rtw8189f_rx_drain(sc);

		if (sc->sc_ih != NULL)
			sdmmc_intr_ack(sc->sc_ih);

		wlan_port_serializer_unlock();
	}

	/* Flush anything still queued. */
	for (;;) {
		MBUFQ_DEQUEUE(&locq, m);
		if (m == NULL)
			break;
		ni = M_GETCTX(m, struct ieee80211_node *);
		if (ni != NULL)
			ieee80211_free_node(ni);
		m_freem(m);
	}
	mutex_enter(&sc->sc_work_mtx);
	for (;;) {
		MBUFQ_DEQUEUE(&sc->sc_txq, m);
		if (m == NULL)
			break;
		ni = M_GETCTX(m, struct ieee80211_node *);
		if (ni != NULL)
			ieee80211_free_node(ni);
		m_freem(m);
	}
	sc->sc_worker = NULL;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_work_mtx);

	/* rtw89 lane lesson: a kthread must kthread_exit(), never return. */
	kthread_exit(0);
}
