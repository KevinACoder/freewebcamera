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

int wlan_port_xmit_iwm(const uint8_t *frame, size_t len) {
	struct iwm_softc *sc = iwm_reg_softc;
	struct ifnet *ifp;
	struct mbuf *m;
	int err;

	if (sc == NULL || frame == NULL || len < sizeof(struct ether_header) ||
	    len > MCLBYTES) {
		return -1;
	}
	ifp = sc->sc_ic.ic_ifp;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL) {
		return -1;
	}
	m->m_len = m->m_pkthdr.len = (int) len;
	memcpy(mtod(m, void *), frame, len);

	IFQ_ENQUEUE(&ifp->if_snd, m, err);
	if (err != 0) {
		m_freem(m);
		return -1;
	}
	if_start_lock(ifp);
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
	printf("iwm state=%s opmode=%d ch=%d fw=%s\n",
	    ic->ic_state >= 0 && ic->ic_state < IEEE80211_S_MAX ?
	        ieee80211_state_name[ic->ic_state] : "?",
	    ic->ic_opmode,
	    ic->ic_curchan != NULL ? ic->ic_curchan->ic_freq : 0,
	    sc->sc_fwname != NULL ? sc->sc_fwname : "(none)");
}

void wlan_iwm_scan_dump(void) {
	if (iwm_reg_softc != NULL) {
		ieee80211_iterate_nodes(&iwm_reg_softc->sc_ic.ic_scan,
		    wlan_print_node_cb, NULL);
	}
}
