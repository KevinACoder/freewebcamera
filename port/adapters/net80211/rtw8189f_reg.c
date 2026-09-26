/*
 * @file
 * @brief rtw8189f chip driver registration for the port interface.
 *
 * rtw8189f_attach() keeps its NetBSD autoconf shape; this adapter
 * plays the role of the SDIO bus attachment: softc allocation, the
 * device shell, and the sdmmc_attach_args built from the claimed
 * wlan_sdio_dev (function-0 CIS and the function number).
 *
 * The verbatim import is compiled into this unit so its static glue
 * (CFATTACH_DECL_NEW tables and the static attach path) stays intact -
 * the same trick the net_80211 library used and this repo's urtwn_reg.c
 * / iwm_reg.c keep for the USB and PCIe lines.
 *
 * @date 22.09.2026
 * @author zhugengyu
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/queue.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/if_inarp.h>
/* the same prologue the verbatim driver carries: net80211 expects the
 * ifnet/ifmedia world to exist before ieee80211_var.h */
#include <net80211/ieee80211_netbsd.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include "port.h"
#include "wlan_port_cmsis.h"
#include <sd/port_sd.h>

/* shell wrappers defined below */
int wlan_rtw8189f_up(void);
void wlan_rtw8189f_dump(void);
void wlan_rtw8189f_scan_dump(void);
int wlan_port_scan_rtw8189f(const uint8_t *ssid, size_t len);
int wlan_port_xmit_rtw8189f(const uint8_t *frame, size_t len);
int wlan_port_get_hwaddr_rtw8189f(uint8_t addr[6]);

/* the verbatim import compiled into this unit so its static glue
 * (CFATTACH_DECL_NEW tables and the static attach path) stays intact */
#include "../../../third-party/net80211/sys/dev/sdmmc/if_rtw8189f.c"

static const struct wlan_sdio_id rtw8189f_sdio_ids[] = {
	{ 0x024c, 0xf179 }, /* RTL8189FTV */
	{ 0, 0 }
};

struct rtw8189f_softc *rtw8189f_reg_softc;

int wlan_rtw8189f_attach(struct wlan_sdio_dev *sdio, void *if_priv);

static int wlan_rtw8189f_attach_bus(void *bus_dev, void *if_priv) {
	return wlan_rtw8189f_attach((struct wlan_sdio_dev *) bus_dev,
	    if_priv);
}

static struct wlan_port_adapter rtw8189f_adapter = {
	.name = "rtw8189f",
	.up = wlan_rtw8189f_up,
	.scan = wlan_port_scan_rtw8189f,
	.xmit = wlan_port_xmit_rtw8189f,
	.get_hwaddr = wlan_port_get_hwaddr_rtw8189f,
	.status_dump = wlan_rtw8189f_dump,
	.scan_dump = wlan_rtw8189f_scan_dump,
};

int wlan_rtw8189f_attach(struct wlan_sdio_dev *sdio, void *if_priv) {
	static unsigned rtw8189f_unit;
	struct rtw8189f_softc *sc;
	struct device *self;
	struct sdmmc_softc *ssc;
	struct sdmmc_function *sf, *fn0;
	struct sdmmc_attach_args saa;

	(void) if_priv;
	sc = kmem_zalloc(sizeof(struct rtw8189f_softc), KM_SLEEP);
	self = kmem_zalloc(sizeof(struct device), KM_SLEEP);
	ssc = kmem_zalloc(sizeof(struct sdmmc_softc), KM_SLEEP);
	sf = kmem_zalloc(sizeof(struct sdmmc_function), KM_SLEEP);
	fn0 = kmem_zalloc(sizeof(struct sdmmc_function), KM_SLEEP);
	snprintf(self->dv_xname, sizeof(self->dv_xname), "rtw8189f%u",
	    rtw8189f_unit++);
	self->dv_private = sc;

	/* the autoconf match walks fn0's CIS through the parent softc */
	ssc->sc_fn0 = fn0;
	fn0->sc = ssc;
	fn0->cis.manufacturer = sdio->vendor;
	fn0->cis.product = sdio->product;
	sf->sc = ssc;
	sf->number = sdio->function;
	sf->sf_port = sdio;
	sdio->port_priv = sf;

	memset(&saa, 0, sizeof(saa));
	saa.sf = sf;

	rtw8189f_attach(self, self, &saa);

	if (!sc->sc_dying && sc->sc_attached) {
		/* remember the first healthy unit for the shell hooks */
		if (rtw8189f_reg_softc == NULL) {
			rtw8189f_reg_softc = sc;
			rtw8189f_adapter.ic = &sc->sc_ic;
			wlan_port_adapter_register(&rtw8189f_adapter);
		}
		return 0;
	}
	return -EIO;
}

void wlan_rtw8189f_detach(void *priv) {
	struct wlan_sdio_dev *sdio = priv;
	(void) sdio;
	/* detach goes through rtw8189f_detach with the stored shells;
	 * hot-unplug does not exist on the soldered module */
}

int wlan_rtw8189f_up(void) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;
	struct ifnet *ifp;

	if (sc == NULL || sc->sc_dying) {
		printf("wlan: no attached rtw8189f device\n");
		return -1;
	}
	ifp = &sc->sc_if;
	if (!(ifp->if_flags & IFF_RUNNING)) {
		ifp->if_flags |= IFF_UP;
		printf("wlan: calling if_init %p (fw + chip init)...\n",
		    (void *) ifp->if_init);
		/* the serializer stands in for the splnet an ioctl-driven
		 * if_init holds on NetBSD */
		wlan_port_serializer_lock();
		ifp->if_init(ifp);
		wlan_port_serializer_unlock();
		printf("wlan: if_init done, flags=%x\n", ifp->if_flags);
	}
	return 0;
}

struct wlan_scan_request {
	uint8_t ssid[IEEE80211_NWID_LEN];
	uint8_t len;
};

static void wlan_scan_start(struct rtw8189f_softc *sc, void *arg) {
	const struct wlan_scan_request *req = arg;
	struct ieee80211com *ic = &sc->sc_ic;

	ic->ic_roaming = IEEE80211_ROAMING_MANUAL;
	/* Like SCAN_REQ, start from INIT so begin_scan resets the channel
	 * bitmap even when the previous manual scan left the state at
	 * SCAN. The INIT leg is chip-idle; the SCAN leg does chip work
	 * (set_channel/dwell) and defers to the driver worker through its
	 * own newstate flag - the worker holds the port serializer for
	 * that leg, so this shell-side leg holds it too. */
	wlan_port_serializer_lock();
	rtw8189f_newstate_cb(sc, IEEE80211_S_INIT, -1);
	memcpy(ic->ic_des_essid, req->ssid, req->len);
	ic->ic_des_esslen = req->len;
	memcpy(ic->ic_chan_active, ic->ic_chan_avail, sizeof(ic->ic_chan_active));
	mutex_enter(&sc->sc_work_mtx);
	sc->sc_nstate = IEEE80211_S_SCAN;
	sc->sc_narg = -1;
	sc->sc_flags |= RTW8189F_F_NEWSTATE;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_work_mtx);
	wlan_port_serializer_unlock();
}

int wlan_port_scan_rtw8189f(const uint8_t *ssid, size_t len) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;
	struct wlan_scan_request req = {0};

	if (sc == NULL || sc->sc_dying || !(sc->sc_if.if_flags & IFF_RUNNING) ||
	    len > sizeof(req.ssid) || (len != 0 && ssid == NULL)) {
		return -1;
	}
	if (len != 0) {
		memcpy(req.ssid, ssid, len);
	}
	req.len = len;
	wlan_scan_start(sc, &req);
	return 0;
}

static void wlan_print_node_cb(void *arg, struct ieee80211_node *ni) {
	(void) arg;
	struct ieee80211_channel *ch = ni->ni_chan;
	/* minilibc snprintf has no "%.*s": copy the essid out and print %s */
	char essid[33];
	unsigned int i;
	unsigned int n = ni->ni_esslen;

	if (n > sizeof(essid) - 1) {
		n = sizeof(essid) - 1;
	}
	for (i = 0; i < n; i++) {
		essid[i] = (char) ni->ni_essid[i];
	}
	essid[n] = '\0';
	printf("  %02x:%02x:%02x:%02x:%02x:%02x  ch=%d  rssi=%u  %s  ssid=%s\n",
	    ni->ni_bssid[0], ni->ni_bssid[1], ni->ni_bssid[2],
	    ni->ni_bssid[3], ni->ni_bssid[4], ni->ni_bssid[5],
	    ch != NULL ? ch->ic_freq : 0, ni->ni_rssi,
	    (ni->ni_capinfo & IEEE80211_CAPINFO_PRIVACY) ? "enc " : "open",
	    essid);
}

void wlan_rtw8189f_dump(void) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;
	struct ieee80211com *ic;

	if (sc == NULL) {
		printf("wlan: no device\n");
		return;
	}
	ic = &sc->sc_ic;
	printf("wlan mac=%s flags=%x mtu=%u\n",
	    ether_sprintf(sc->sc_mac_addr), sc->sc_if.if_flags,
	    sc->sc_if.if_mtu);
	if (ic->ic_bss != NULL) {
		printf("wlan bssid=%s ni_flags=%x caps=%x\n",
		    ether_sprintf(ic->ic_bss->ni_bssid), ic->ic_bss->ni_flags,
		    ic->ic_caps);
	}
	printf("wlan stats tx=%llu txerr=%llu rx=%llu rxerr=%llu\n",
	    (unsigned long long)sc->sc_if.if_data.if_opackets,
	    (unsigned long long)sc->sc_if.if_data.if_oerrors,
	    (unsigned long long)sc->sc_if.if_data.if_ipackets,
	    (unsigned long long)sc->sc_if.if_data.if_ierrors);
	printf("wlan counters rx_frames=%u rx_beacons=%u rx_errors=%u "
	    "tx_frames=%u txrpt=%u ucast_rx=%u iqk_done=%u\n",
	    sc->sc_rx_frames, sc->sc_rx_beacons, sc->sc_rx_errors,
	    sc->sc_tx_frames, sc->sc_txrpt_seq, sc->sc_ucast_rx,
	    (unsigned) sc->sc_iqk_done);
	printf("rtw8189f state=%s opmode=%d ch=%d fw_ready=%u chip_ready=%u\n",
	    ic->ic_state >= 0 && ic->ic_state < IEEE80211_S_MAX ?
	        ieee80211_state_name[ic->ic_state] : "?",
	    ic->ic_opmode,
	    ic->ic_curchan != NULL ? ic->ic_curchan->ic_freq : 0,
	    (unsigned) sc->sc_fw_ready, (unsigned) sc->sc_chip_ready);
}

void wlan_rtw8189f_scan_dump(void) {
	if (rtw8189f_reg_softc != NULL) {
		ieee80211_iterate_nodes(&rtw8189f_reg_softc->sc_ic.ic_scan,
		    wlan_print_node_cb, NULL);
	}
}

int wlan_port_xmit_rtw8189f(const uint8_t *frame, size_t len) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;
	struct ifnet *ifp;
	struct mbuf *m;
	int err;

	if (sc == NULL || frame == NULL || len < sizeof(struct ether_header) ||
	    len > MCLBYTES) {
		return -1;
	}
	ifp = &sc->sc_if;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL) {
		return -1;
	}
	m->m_len = m->m_pkthdr.len = (int) len;
	memcpy(mtod(m, void *), frame, len);

	/* IFQ_ENQUEUE takes the port serializer (see compat net/if.h):
	 * the driver worker dequeues from this same queue, and NetBSD
	 * gets the same exclusion from splnet(). */
	IFQ_ENQUEUE(&ifp->if_snd, m, err);
	if (err != 0) {
		m_freem(m);
		return -1;
	}
	if_start_lock(ifp);
	return (int) len;
}

int wlan_port_get_hwaddr_rtw8189f(uint8_t addr[6]) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;
	struct ifnet *ifp;

	if (sc == NULL) {
		return -1;
	}
	ifp = &sc->sc_if;
	if (ifp->if_sadl == NULL) {
		return -1;
	}
	memcpy(addr, CLLADDR(ifp->if_sadl), 6);
	return 0;
}

const struct wlan_chip_driver rtw8189f_driver = {
	.name = "rtw8189f",
	.bus = WLAN_BUS_SDIO,
	.sdio_ids = rtw8189f_sdio_ids,
	.attach = wlan_rtw8189f_attach_bus,
	.detach = wlan_rtw8189f_detach,
	.stop = NULL,
};

/* --- register-level debug access (shell "wlan reg") ----------------------- */

int wlan_rtw8189f_reg_read(unsigned addr, unsigned *val) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;

	if (sc == NULL || sc->sc_dying || val == NULL) {
		return -1;
	}
	*val = rtw8189f_mac_read_4(sc, (uint32_t) addr);
	return 0;
}

int wlan_rtw8189f_reg_write(unsigned addr, unsigned val) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;

	if (sc == NULL || sc->sc_dying ||
	    !(sc->sc_if.if_flags & IFF_RUNNING)) {
		return -1;
	}
	wlan_port_serializer_lock();
	rtw8189f_mac_write_4(sc, (uint32_t) addr, (uint32_t) val);
	wlan_port_serializer_unlock();
	return 0;
}

/* TX-queue forensics for the mid-stall state (the KI-040 chase): the
 * 8188F MAC keeps the old Realtek layout, so the page counters and
 * queue head/tail pointers sit at the same addresses the urtwn lane
 * dumps. Read twice ~1 s apart and compare: pages accumulating = queue
 * jam, TXPAUSE set = firmware valve, static head/tail = dead pipe. */
void wlan_rtw8189f_txq_dump(void) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;
	static const struct { uint32_t addr; const char *name; } regs[] = {
		{ 0x0100, "CR" },
		{ 0x0104, "PBP" },
		{ 0x0118, "TRXFF_STATUS" },
		{ 0x01cc, "HMETFR" },
		{ 0x0200, "RQPN" },
		{ 0x0204, "FIFOPAGE" },
		{ 0x0208, "TDECTRL" },
		{ 0x020c, "TXDMA_OFFSET_CHK" },
		{ 0x0210, "TXDMA_STATUS" },
		{ 0x0214, "RQPN_NPQ" },
		{ 0x0400, "VOQ" },
		{ 0x0404, "VIQ" },
		{ 0x0408, "BEQ" },
		{ 0x040c, "BKQ" },
		{ 0x0410, "MGQ" },
		{ 0x0414, "HGQ" },
		{ 0x0418, "BCNQ" },
		{ 0x041c, "CPU_MGQ" },
	};
	unsigned i;

	if (sc == NULL || sc->sc_dying) {
		printf("wlan: no attached rtw8189f device\n");
		return;
	}
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		printf("rtw8189f %-18s [0x%03x] = 0x%08x\n", regs[i].name,
		    regs[i].addr, rtw8189f_mac_read_4(sc, regs[i].addr));
	}
	printf("rtw8189f %-18s [0x522] = 0x%02x\n", "TXPAUSE",
	    rtw8189f_mac_read_1(sc, 0x0522));
}

/* net80211 RX-path error counters (ic_stats). The live ones for the
 * TCP-downlink chase: dup removal, CCMP replay, demic/decap failures,
 * address-filter rejects and mbuf starvation. All zeros while frames
 * keep flowing means the drop is downstream (lwIP bridge / tcpip). */
void wlan_rtw8189f_icstats_dump(void) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;
	struct ieee80211_stats *st;

	if (sc == NULL) {
		printf("wlan: no device\n");
		return;
	}
	st = &sc->sc_ic.ic_stats;
	printf("wlan icstats rx: dup=%u ccmpreplay=%u tkipreplay=%u "
	    "wepfail=%u decap=%u nobuf=%u\n",
	    st->is_rx_dup, st->is_rx_ccmpreplay, st->is_rx_tkipreplay,
	    st->is_rx_wepfail, st->is_rx_decap, st->is_rx_nobuf);
	printf("wlan icstats rx: wrongbss=%u notassoc=%u unauth=%u "
	    "tooshort=%u badversion=%u mcastecho=%u wrongdir=%u\n",
	    st->is_rx_wrongbss, st->is_rx_notassoc, st->is_rx_unauth,
	    st->is_rx_tooshort, st->is_rx_badversion, st->is_rx_mcastecho,
	    st->is_rx_wrongdir);
	printf("wlan icstats rx: decryptcrc=%u badkeyid=%u noprivacy=%u "
	    "unencrypted=%u deauth=%u disassoc=%u\n",
	    st->is_rx_decryptcrc, st->is_rx_badkeyid, st->is_rx_noprivacy,
	    st->is_rx_unencrypted, st->is_rx_deauth, st->is_rx_disassoc);
	printf("wlan icstats tx: nobuf=%u nonode=%u noheadroom=%u "
	    "badcipher=%u frags=%u\n",
	    st->is_tx_nobuf, st->is_tx_nonode, st->is_tx_noheadroom,
	    st->is_tx_badcipher, st->is_tx_frags);
}

/* SDIO-local (DeviceID 0) registers: the interrupt/fifo state the MAC
 * window cannot show. RX0_REQ_LEN is the live pending-RX length, HISR
 * bit0 the pending RX_REQUEST - read twice ~1 s apart: a wedged RX DMA
 * shows as REQ_LEN pinned nonzero (or zero with the server still
 * sending) and HISR stuck. Byte reads only, like the power-on selftest. */
static unsigned wlan_rtw8189f_sdlocal_read_4(struct rtw8189f_softc *sc,
    uint16_t reg) {
	return (unsigned) rtw8189f_sdiolocal_read_1(sc, reg) |
	    ((unsigned) rtw8189f_sdiolocal_read_1(sc, reg + 1) << 8) |
	    ((unsigned) rtw8189f_sdiolocal_read_1(sc, reg + 2) << 16) |
	    ((unsigned) rtw8189f_sdiolocal_read_1(sc, reg + 3) << 24);
}

void wlan_rtw8189f_sdreg_dump(void) {
	struct rtw8189f_softc *sc = rtw8189f_reg_softc;

	if (sc == NULL || sc->sc_dying) {
		printf("wlan: no attached rtw8189f device\n");
		return;
	}
	printf("rtw8189f sdreg TX_CTRL     [0x000] = 0x%08x\n",
	    wlan_rtw8189f_sdlocal_read_4(sc, 0x0000));
	printf("rtw8189f sdreg HIMR        [0x014] = 0x%08x\n",
	    wlan_rtw8189f_sdlocal_read_4(sc, RTW8189F_SDIO_REG_HIMR));
	printf("rtw8189f sdreg HISR        [0x018] = 0x%08x\n",
	    wlan_rtw8189f_sdlocal_read_4(sc, RTW8189F_SDIO_REG_HISR));
	printf("rtw8189f sdreg RX0_REQ_LEN [0x01c] = %u\n",
	    (unsigned) rtw8189f_sdiolocal_read_1(sc,
	        RTW8189F_SDIO_REG_RX0_REQ_LEN) |
	    ((unsigned) rtw8189f_sdiolocal_read_1(sc,
	        RTW8189F_SDIO_REG_RX0_REQ_LEN + 1) << 8));
	printf("rtw8189f sdreg FREE_TXPG   [0x020] = 0x%08x\n",
	    wlan_rtw8189f_sdlocal_read_4(sc, RTW8189F_SDIO_REG_FREE_TXPG));
}
