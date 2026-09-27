/*
 * @file
 * @brief urtwn chip driver registration for the port interface.
 *
 * The verbatim NetBSD import is compiled into this unit so its static
 * glue (CFATTACH_DECL_NEW tables) stays intact - the same trick the
 * net_80211 library used. Attach runs through the real usbdi autoconf
 * chain (usb_subr -> config_found -> urtwn_ca); this TU only adds the
 * post-attach registry note and the shell-facing wrappers (up, scan,
 * xmit, hwaddr, dumps, register forensics).
 *
 * The 88E firmware-maintenance hook of the net_80211 line is NOT
 * carried over: it was proven net-harmful there (board evidence
 * 20260923-iperf3-sustained-tx-wedge) and is irrelevant without an
 * association line.
 *
 * @date 25.09.2026
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

/* shell wrappers defined below */
int wlan_urtwn_up(void);
int wlan_port_scan_urtwn(const uint8_t *ssid, size_t len);
int wlan_port_xmit_urtwn(const uint8_t *frame, size_t len);
int wlan_port_get_hwaddr_urtwn(uint8_t addr[6]);
void wlan_urtwn_dump(void);
void wlan_urtwn_scan_dump(void);
void wlan_urtwn_chanmap_dump(void);

/* the verbatim import compiled into this unit so its static glue
 * (CFATTACH_DECL_NEW tables) stays intact */
#include "../../../third-party/libbsd/sys/dev/usb/if_urtwn.c"

struct urtwn_softc *urtwn_reg_softc;

/* the shell-facing adapter table (see port.h) */
static struct wlan_port_adapter urtwn_adapter = {
	.name = "urtwn",
	.up = wlan_urtwn_up,
	.scan = wlan_port_scan_urtwn,
	.xmit = wlan_port_xmit_urtwn,
	.get_hwaddr = wlan_port_get_hwaddr_urtwn,
	.status_dump = wlan_urtwn_dump,
	.scan_dump = wlan_urtwn_scan_dump,
};

/* called by the autoconf glue after every successful ca_attach */
void
wlan_port_post_attach(device_t dev)
{
	struct urtwn_softc *sc;

	if (strncmp(device_xname(dev), "urtwn", 5) != 0) {
		return;
	}
	sc = device_private(dev);
	if (sc == NULL || sc->sc_dying ||
	    !ISSET(sc->sc_flags, URTWN_FLAG_ATTACHED)) {
		printf("wlan: %s attached but not healthy\n",
		    device_xname(dev));
		return;
	}
	if (urtwn_reg_softc == NULL) {
		urtwn_reg_softc = sc;
		urtwn_adapter.ic = &sc->sc_ic;
		wlan_port_adapter_register(&urtwn_adapter);
		printf("wlan: %s registered (mac %s)\n", device_xname(dev),
		    ether_sprintf(sc->sc_ic.ic_myaddr));
	}
}

/* ------------------------------------------------------------------ */
/* shell wrappers                                                     */

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

int wlan_urtwn_up(void) {
	struct urtwn_softc *sc = urtwn_reg_softc;
	struct ifnet *ifp;

	if (sc == NULL || sc->sc_dying) {
		printf("wlan: no attached urtwn device\n");
		return -1;
	}
	ifp = &sc->sc_if;
	if (!(ifp->if_flags & IFF_RUNNING)) {
		ifp->if_flags |= IFF_UP;
		printf("wlan: calling if_init %p (fw load + power on, ~10s)...\n",
		    (void *) ifp->if_init);
		ifp->if_init(ifp);
		printf("wlan: if_init done, flags=%x\n", ifp->if_flags);
	}
	return 0;
}

struct wlan_scan_request {
	uint8_t ssid[IEEE80211_NWID_LEN];
	uint8_t len;
};

static void wlan_scan_start(struct urtwn_softc *sc, void *arg) {
	const struct wlan_scan_request *req = arg;
	struct ieee80211com *ic = &sc->sc_ic;
	struct urtwn_cmd_newstate cmd = { .state = IEEE80211_S_INIT, .arg = -1 };

	ic->ic_roaming = IEEE80211_ROAMING_MANUAL;
	/* Like SCAN_REQ, start from INIT so begin_scan resets the channel
	 * bitmap even when the previous manual scan left the state at SCAN.
	 * Run both transitions on the driver worker to preserve their order. */
	urtwn_newstate_cb(sc, &cmd);
	memcpy(ic->ic_des_essid, req->ssid, req->len);
	ic->ic_des_esslen = req->len;
	memcpy(ic->ic_chan_active, ic->ic_chan_avail, sizeof(ic->ic_chan_active));
	cmd.state = IEEE80211_S_SCAN;
	urtwn_newstate_cb(sc, &cmd);
}

int wlan_port_scan_urtwn(const uint8_t *ssid, size_t len) {
	struct urtwn_softc *sc = urtwn_reg_softc;
	struct wlan_scan_request req = {0};

	if (sc == NULL || sc->sc_dying || !(sc->sc_if.if_flags & IFF_RUNNING) ||
	    len > sizeof(req.ssid) || (len != 0 && ssid == NULL)) {
		return -1;
	}
	if (len != 0) {
		memcpy(req.ssid, ssid, len);
	}
	req.len = len;
	urtwn_do_async(sc, wlan_scan_start, &req, sizeof(req));
	return 0;
}

void wlan_urtwn_dump(void) {
	struct urtwn_softc *sc = urtwn_reg_softc;
	struct ieee80211com *ic;

	if (sc == NULL) {
		printf("wlan: no device\n");
		return;
	}
	ic = &sc->sc_ic;
	printf("wlan mac=%s flags=%x mtu=%u\n",
	    ether_sprintf(ic->ic_myaddr), sc->sc_if.if_flags, sc->sc_if.if_mtu);
	if (ic->ic_bss == NULL) {
		printf("wlan: no BSS\n");
	} else {
		printf("wlan bssid=%s ni_flags=%x caps=%x\n",
		    ether_sprintf(ic->ic_bss->ni_bssid), ic->ic_bss->ni_flags,
		    ic->ic_caps);
	}
	printf("wlan stats tx=%llu txerr=%llu rx=%llu rxerr=%llu\n",
	    (unsigned long long)sc->sc_if.if_data.if_opackets,
	    (unsigned long long)sc->sc_if.if_data.if_oerrors,
	    (unsigned long long)sc->sc_if.if_data.if_ipackets,
	    (unsigned long long)sc->sc_if.if_data.if_ierrors);
	if (sc->sc_if.if_flags & IFF_RUNNING) {
		printf("urtwn RCR=%08x RXFLTMAP=%04x/%04x/%04x SECCFG=%02x\n",
		    urtwn_read_4(sc, R92C_RCR), urtwn_read_2(sc, R92C_RXFLTMAP0),
		    urtwn_read_2(sc, R92C_RXFLTMAP1), urtwn_read_2(sc, R92C_RXFLTMAP2),
		    urtwn_read_1(sc, R92C_SECCFG));
	}
	printf("urtwn state=%s opmode=%d ch=%d\n",
	    ic->ic_state >= 0 && ic->ic_state < IEEE80211_S_MAX ?
	        ieee80211_state_name[ic->ic_state] : "?",
	    ic->ic_opmode,
	    ic->ic_curchan != NULL ? ic->ic_curchan->ic_freq : 0);
}

void wlan_urtwn_scan_dump(void) {
	if (urtwn_reg_softc != NULL) {
		ieee80211_iterate_nodes(&urtwn_reg_softc->sc_ic.ic_scan,
		    wlan_print_node_cb, NULL);
	}
}

/* first-scan forensics: the state machine, the scan bitmap and the host
 * command ring in one screen - which channel the heartbeat died on,
 * whether the bitmap still has channels left, and whether the cmd ring
 * the re-arm depends on is backed up */
void wlan_urtwn_chanmap_dump(void) {
	struct urtwn_softc *sc = urtwn_reg_softc;
	struct ieee80211com *ic;
	int chan, first;

	if (sc == NULL) {
		printf("wlan: no device\n");
		return;
	}
	ic = &sc->sc_ic;
	printf("urtwn state=%s opmode=%d curchan=%u fscan=%d\n",
	    ic->ic_state >= 0 && ic->ic_state < IEEE80211_S_MAX ?
	        ieee80211_state_name[ic->ic_state] : "?",
	    ic->ic_opmode,
	    ic->ic_curchan != NULL ? ic->ic_curchan->ic_freq : 0,
	    (ic->ic_flags & IEEE80211_F_SCAN) ? 1 : 0);
	printf("scan bitmap:\n");
	for (chan = 1, first = 1; chan < (int) (sizeof(ic->ic_chan_scan) * 8);
	    chan++) {
		if ((ic->ic_chan_scan[chan >> 3] & (1 << (chan & 7))) != 0) {
			printf("%s%d", first ? "  scan:" : ",", chan);
			first = 0;
		}
	}
	printf("%s\n", first ? "  scan: (empty)" : "");
	for (chan = 1, first = 1;
	    chan < (int) (sizeof(ic->ic_chan_active) * 8); chan++) {
		if ((ic->ic_chan_active[chan >> 3] & (1 << (chan & 7))) != 0) {
			printf("%s%d", first ? "  active:" : ",", chan);
			first = 0;
		}
	}
	printf("%s\n", first ? "  active: (empty)" : "");
	printf("cmd ring cur=%d next=%d queued=%d/%d\n",
	    sc->cmdq.cur, sc->cmdq.next, sc->cmdq.queued,
	    URTWN_HOST_CMD_RING_COUNT);
}

/* --- register-level debug access (shell "wlan reg") ----------------------- */

int wlan_urtwn_reg_read(unsigned addr, unsigned *val) {
	struct urtwn_softc *sc = urtwn_reg_softc;

	if (sc == NULL || sc->sc_dying || val == NULL) {
		return -1;
	}
	/* synchronous ep0 access straight from the calling thread; it
	 * only serializes against other control transfers, never the
	 * bulk paths */
	*val = urtwn_read_4(sc, (uint16_t) (addr & 0xffffu));
	return 0;
}

int wlan_urtwn_reg_write(unsigned addr, unsigned val) {
	struct urtwn_softc *sc = urtwn_reg_softc;

	if (sc == NULL || sc->sc_dying ||
	    !(sc->sc_if.if_flags & IFF_RUNNING)) {
		return -1;
	}
	/* the driver mutates registers under sc_write_mtx from the usb
	 * taskq; a shell-context write needs the same discipline plus
	 * the port serializer so taskq callbacks cannot interleave */
	wlan_port_serializer_lock();
	mutex_enter(&sc->sc_write_mtx);
	urtwn_write_4(sc, (uint16_t) (addr & 0xffffu), val);
	mutex_exit(&sc->sc_write_mtx);
	wlan_port_serializer_unlock();
	return 0;
}

/* TX-queue forensics for the mid-stall state (kept from the net_80211
 * line; read twice ~1 s apart and compare). */
void wlan_urtwn_txq_dump(void) {
	struct urtwn_softc *sc = urtwn_reg_softc;
	static const struct { uint16_t addr; const char *name; } regs[] = {
		{ R92C_CR, "CR" },
		{ R92C_PBP, "PBP" },
		{ R92C_TRXFF_STATUS, "TRXFF_STATUS" },
		{ R92C_HMETFR, "HMETFR" },
		{ R92C_RQPN, "RQPN" },
		{ R92C_FIFOPAGE, "FIFOPAGE" },
		{ R92C_TDECTRL, "TDECTRL" },
		{ R92C_TXDMA_OFFSET_CHK, "TXDMA_OFFSET_CHK" },
		{ R92C_TXDMA_STATUS, "TXDMA_STATUS" },
		{ R92C_RQPN_NPQ, "RQPN_NPQ" },
		{ R92C_VOQ_INFORMATION, "VOQ_INFO" },
		{ R92C_VIQ_INFORMATION, "VIQ_INFO" },
		{ R92C_BEQ_INFORMATION, "BEQ_INFO" },
		{ R92C_BKQ_INFORMATION, "BKQ_INFO" },
		{ R92C_MGQ_INFORMATION, "MGQ_INFO" },
		{ R92C_HGQ_INFORMATION, "HGQ_INFO" },
		{ R92C_BCNQ_INFORMATION, "BCNQ_INFO" },
		{ R92C_CPU_MGQ_INFORMATION, "CPU_MGQ_INFO" },
	};
	unsigned i;

	if (sc == NULL || sc->sc_dying) {
		printf("wlan: no attached urtwn device\n");
		return;
	}
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		printf("urtwn %-18s [0x%03x] = 0x%08x\n", regs[i].name,
		    regs[i].addr, urtwn_read_4(sc, regs[i].addr));
	}
	printf("urtwn %-18s [0x522] = 0x%02x\n", "TXPAUSE",
	    urtwn_read_1(sc, R92C_TXPAUSE));
}

int wlan_port_xmit_urtwn(const uint8_t *frame, size_t len) {
	struct urtwn_softc *sc = urtwn_reg_softc;
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
	 * the driver completion path dequeues from this same queue on
	 * the usbdi worker core, and NetBSD gets the same exclusion
	 * from splnet(). */
	IFQ_ENQUEUE(&ifp->if_snd, m, err);
	if (err != 0) {
		m_freem(m);
		return -1;
	}
	if_start_lock(ifp);
	return (int) len;
}

int wlan_port_get_hwaddr_urtwn(uint8_t addr[6]) {
	struct urtwn_softc *sc = urtwn_reg_softc;
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
