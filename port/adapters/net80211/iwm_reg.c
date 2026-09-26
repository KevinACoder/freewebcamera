/*
 * @file
 * @brief iwm chip driver registration for the port interface.
 *
 * The verbatim NetBSD import is compiled into this unit so its static
 * glue (CFATTACH_DECL_NEW tables) stays intact - the same trick
 * urtwn_reg.c uses for the USB line.  Attach runs from the native
 * pcie glue (pcie_glue.c config_founds the "pci" iattr with a
 * hand-filled pci_attach_args; no NetBSD pci core, no fdt world);
 * this TU adds the post-attach registry note and the shell-facing
 * wrappers (up, scan, xmit, hwaddr, dumps).
 *
 * iwm runs its newstate transitions through its own workqueue
 * (iwm_newstate_cb), so the scan request can be issued from the shell
 * thread directly - unlike the USB line, no driver-worker trampoline
 * is needed here.
 *
 * @date 26.09.2026
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

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_netbsd.h>

#include "../port.h"

/* shell-facing wrappers (defined below the import) */
int wlan_iwm_up(void);
int wlan_port_scan_iwm(const uint8_t *ssid, size_t len);
int wlan_port_xmit_iwm(const uint8_t *frame, size_t len);
int wlan_port_get_hwaddr_iwm(uint8_t addr[6]);
void wlan_iwm_dump(void);
void wlan_iwm_scan_dump(void);

/* the verbatim import compiled into this unit so its static glue
 * (CFATTACH_DECL_NEW tables) stays intact */
#include "../../../third-party/net80211/sys/dev/pci/if_iwm.c"

struct iwm_softc *iwm_reg_softc;

/* the shell-facing adapter table (see port.h) */
static struct wlan_port_adapter iwm_adapter = {
	.name = "iwm",
	.up = wlan_iwm_up,
	.scan = wlan_port_scan_iwm,
	.xmit = wlan_port_xmit_iwm,
	.get_hwaddr = wlan_port_get_hwaddr_iwm,
	.status_dump = wlan_iwm_dump,
	.scan_dump = wlan_iwm_scan_dump,
};

/* called by the autoconf glue after every successful ca_attach
 * (bsd_autoconf.c dispatches per-driver; urtwn_reg.c carries the usb
 * counterpart) */
void
wlan_port_post_attach_iwm(device_t dev)
{
	struct iwm_softc *sc;

	if (strncmp(device_xname(dev), "iwm", 3) != 0) {
		return;
	}
	sc = device_private(dev);
	if (sc == NULL) {
		printf("wlan: %s attached but softc missing\n",
		    device_xname(dev));
		return;
	}
	if (iwm_reg_softc == NULL) {
		/*
		 * Only a driver that ran attach to the end may enter the
		 * service table.  A half-attached iwm (no BAR mapping, no
		 * interrupt) still reaches this hook, and its deferred
		 * tasks would then run against an unpopulated softc.
		 * sc_ih is set by the attach's final step, the interrupt
		 * establishment; ic_ifp by the ifnet attach before it.
		 */
		if (sc->sc_ih == NULL || sc->sc_ic.ic_ifp == NULL) {
			printf("wlan: %s attach incomplete, not registered\n",
			    device_xname(dev));
			return;
		}
		iwm_reg_softc = sc;
		iwm_adapter.ic = &sc->sc_ic;
		wlan_port_adapter_register(&iwm_adapter);
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

int wlan_iwm_up(void) {
	struct iwm_softc *sc = iwm_reg_softc;
	struct ifnet *ifp;

	if (sc == NULL) {
		printf("wlan: no attached iwm device\n");
		return -1;
	}
	ifp = sc->sc_ic.ic_ifp;
	if (!(ifp->if_flags & IFF_RUNNING)) {
		ifp->if_flags |= IFF_UP;
		printf("wlan: calling if_init %p (fw load + power on, ~10s)...\n",
		    (void *) ifp->if_init);
		ifp->if_init(ifp);
		printf("wlan: if_init done, flags=%x\n", ifp->if_flags);
	}
	return 0;
}

int wlan_port_scan_iwm(const uint8_t *ssid, size_t len) {
	struct iwm_softc *sc = iwm_reg_softc;
	struct ieee80211com *ic;

	if (sc == NULL || !(sc->sc_ic.ic_ifp->if_flags & IFF_RUNNING) ||
	    len > IEEE80211_NWID_LEN || (len != 0 && ssid == NULL)) {
		return -1;
	}
	ic = &sc->sc_ic;
	ic->ic_roaming = IEEE80211_ROAMING_MANUAL;
	if (len != 0) {
		memcpy(ic->ic_des_essid, ssid, len);
	}
	ic->ic_des_esslen = (uint8_t) len;
	memcpy(ic->ic_chan_active, ic->ic_chan_avail,
	    sizeof(ic->ic_chan_active));
	/* iwm_newstate queues the firmware scan on its own workqueue and
	 * WLAN_PORT_SCAN_DONE comes back through the port event hook */
	(*ic->ic_newstate)(ic, IEEE80211_S_SCAN, -1);
	return 0;
}

/* TX exit reasons.  The netif surfaces every failure to lwIP as ERR_IF,
 * so when a send dies the reason has to be counted here (it is the only
 * thing that tells "no mbuf left" apart from "bad length"). */
static unsigned wlan_iwm_tx_ok, wlan_iwm_tx_no_softc, wlan_iwm_tx_badlen,
    wlan_iwm_tx_nombuf, wlan_iwm_tx_ifq;

int wlan_port_xmit_iwm(const uint8_t *frame, size_t len) {
	struct iwm_softc *sc = iwm_reg_softc;
	struct ifnet *ifp;
	struct mbuf *m;
	int err;

	if (sc == NULL) {
		wlan_iwm_tx_no_softc++;
		return -1;
	}
	if (frame == NULL || len < sizeof(struct ether_header) ||
	    len > MCLBYTES) {
		wlan_iwm_tx_badlen++;
		return -1;
	}
	ifp = sc->sc_ic.ic_ifp;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL) {
		wlan_iwm_tx_nombuf++;
		return -1;
	}
	m->m_len = m->m_pkthdr.len = (int) len;
	memcpy(mtod(m, void *), frame, len);

	IFQ_ENQUEUE(&ifp->if_snd, m, err);
	if (err != 0) {
		wlan_iwm_tx_ifq++;
		m_freem(m);
		return -1;
	}
	if_start_lock(ifp);
	wlan_iwm_tx_ok++;
	return (int) len;
}

int wlan_port_get_hwaddr_iwm(uint8_t addr[6]) {
	struct iwm_softc *sc = iwm_reg_softc;
	struct ifnet *ifp;

	if (sc == NULL) {
		return -1;
	}
	ifp = sc->sc_ic.ic_ifp;
	if (ifp->if_sadl == NULL) {
		return -1;
	}
	memcpy(addr, CLLADDR(ifp->if_sadl), 6);
	return 0;
}

void wlan_iwm_dump(void) {
	struct iwm_softc *sc = iwm_reg_softc;
	struct ieee80211com *ic;

	if (sc == NULL) {
		printf("wlan: no device\n");
		return;
	}
	ic = &sc->sc_ic;
	printf("wlan mac=%s flags=%x mtu=%u\n",
	    ether_sprintf(ic->ic_myaddr), sc->sc_ic.ic_ifp->if_flags, sc->sc_ic.ic_ifp->if_mtu);
	if (ic->ic_bss == NULL) {
		printf("wlan: no BSS\n");
	} else {
		printf("wlan bssid=%s ni_flags=%x caps=%x\n",
		    ether_sprintf(ic->ic_bss->ni_bssid), ic->ic_bss->ni_flags,
		    ic->ic_caps);
	}
	printf("wlan stats tx=%llu txerr=%llu rx=%llu rxerr=%llu\n",
	    (unsigned long long)sc->sc_ic.ic_ifp->if_data.if_opackets,
	    (unsigned long long)sc->sc_ic.ic_ifp->if_data.if_oerrors,
	    (unsigned long long)sc->sc_ic.ic_ifp->if_data.if_ipackets,
	    (unsigned long long)sc->sc_ic.ic_ifp->if_data.if_ierrors);
	/* The RX pipeline and the TX exit reasons, in one line each: these
	 * are the port-side measurements that decide whether a dead data
	 * path is "the device stopped delivering" (rx counters frozen),
	 * "we drop what it delivers" (phy_bad/crc_bad climbing) or "the
	 * send never got past the mbuf allocation" (tx nombuf). */
	printf("iwm rx dbg: notif=%u entries=%u delivered=%u phy_bad=%u "
	    "crc_bad=%u rearm_fail=%u\n",
	    iwm_dbg_notif_calls, iwm_dbg_rx_entries, iwm_dbg_rx_delivered,
	    iwm_dbg_rx_phy_bad, iwm_dbg_rx_crc_bad, iwm_dbg_rx_rearm_fail);
	printf("iwm rx buf: allocs=%u nombuf=%u noext=%u mapfail=%u\n",
	    iwm_dbg_rx_allocs, iwm_dbg_rx_nombuf, iwm_dbg_rx_noext,
	    iwm_dbg_rx_mapfail);
	{
		extern unsigned int wlan_mbuf_pool_low, wlan_mbuf_pool_short,
		    wlan_mbuf_pool_dups;

		printf("wlan mbuf pool: low=%u short=%u dup=%u\n",
		    wlan_mbuf_pool_low, wlan_mbuf_pool_short,
		    wlan_mbuf_pool_dups);
	}
	printf("iwm tx dbg: ok=%u no_sc=%u badlen=%u nombuf=%u ifq=%u snd_q=%d\n",
	    wlan_iwm_tx_ok, wlan_iwm_tx_no_softc, wlan_iwm_tx_badlen,
	    wlan_iwm_tx_nombuf, wlan_iwm_tx_ifq, ic->ic_ifp->if_snd.ifq_len);
	printf("iwm state=%s opmode=%d ch=%d fw=%s\n",
	    ic->ic_state >= 0 && ic->ic_state < IEEE80211_S_MAX ?
	        ieee80211_state_name[ic->ic_state] : "?",
	    ic->ic_opmode,
	    ic->ic_curchan != NULL ? ic->ic_curchan->ic_freq : 0,
	    sc->sc_fwname != NULL ? sc->sc_fwname : "(none)");
	/* The reset/scan/PM state that decides whether frames flow: ic_flags
	 * carries IEEE80211_F_SCAN, sc_flags carries IWM_FLAG_SCANNING (the
	 * firmware still sweeping channels) and IWM_FLAG_STOPPED, and
	 * F_PMGTON is the net80211 power-management enable that the AP's PS
	 * bookkeeping keys off. */
	printf("iwm flags ic=%08x sc=%08x scanning=%d stopped=%d psm=%d\n",
	    (unsigned) ic->ic_flags, (unsigned) sc->sc_flags,
	    ISSET(sc->sc_flags, IWM_FLAG_SCANNING) ? 1 : 0,
	    ISSET(sc->sc_flags, IWM_FLAG_STOPPED) ? 1 : 0,
	    (ic->ic_flags & IEEE80211_F_PMGTON) != 0);
	/* Rate adaptation health: ni_txrate is the AMRR-chosen index into
	 * the BSS rate set (this NO_HT build's only control of the firmware
	 * LQ table), and in_amn.txcnt/retrycnt are the per-completion
	 * counters AMRR chooses from. txcnt frozen = completions not
	 * feeding AMRR; ni_txrate pinned at 0 with txcnt growing = the
	 * calib callout is not choosing. */
	if (ic->ic_bss != NULL) {
		struct iwm_node *in = (struct iwm_node *) ic->ic_bss;
		struct ieee80211_rateset *rs = &in->in_ni.ni_rates;
		int idx = in->in_ni.ni_txrate;

		printf("iwm amrr: txrate=%d/%u rate=%d txcnt=%lu retry=%lu\n",
		    idx, rs->rs_nrates,
		    (idx >= 0 && idx < rs->rs_nrates) ?
		        (rs->rs_rates[idx] & IEEE80211_RATE_VAL) : -1,
		    (unsigned long) in->in_amn.amn_txcnt,
		    (unsigned long) in->in_amn.amn_retrycnt);
		{
			extern unsigned iwm_dbg_calib_ticks,
			    iwm_dbg_calib_choose, iwm_dbg_tx_status,
			    iwm_dbg_tx_failack_sum, iwm_dbg_tx_status_err,
			    iwm_dbg_notif_garbage, iwm_dbg_notif_unhandled,
			    iwm_dbg_last_unhandled_code;
			extern int iwm_dbg_tx_last_failack;

			printf("iwm amrr2: calib=%u choose=%u txsts=%u "
			    "fsum=%u ferr=%u flast=%d garbage=%u unhand=%u "
			    "last=0x%x\n",
			    iwm_dbg_calib_ticks, iwm_dbg_calib_choose,
			    iwm_dbg_tx_status, iwm_dbg_tx_failack_sum,
			    iwm_dbg_tx_status_err, iwm_dbg_tx_last_failack,
			    iwm_dbg_notif_garbage,
			    iwm_dbg_notif_unhandled,
			    iwm_dbg_last_unhandled_code);
		}
	}
}

void wlan_iwm_scan_dump(void) {
	if (iwm_reg_softc != NULL) {
		ieee80211_iterate_nodes(&iwm_reg_softc->sc_ic.ic_scan,
		    wlan_print_node_cb, NULL);
	}
}
