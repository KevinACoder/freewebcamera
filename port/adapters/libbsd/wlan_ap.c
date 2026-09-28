/*
 * @file
 * @brief HOSTAP bring-up for the net80211 port: the ioctl-less opmode
 * switch the pre-vap stack expects.
 *
 * ieee80211_ioctl.c is deliberately outside the compiled set, so the
 * opmode/media path does not exist.  This is the same "write the ic_*
 * fields, then arm new_state" contract the supplicant bridge uses for
 * station joins (driver_net80211.c freertos_associate), shaped for the
 * AP: opmode + essid + des_chan + bintval, then INIT -> SCAN.  The
 * stack's SCAN-from-INIT transition runs ieee80211_create_ibss() for a
 * HOSTAP with a fixed channel (ieee80211_proto.c), which builds the BSS
 * on our own MAC and drives RUN - where the driver takes over (beacon
 * template upload, nettype AP).
 *
 * The caller is the shell thread; the chip work still runs on the
 * driver's worker exactly like a scan or a join.
 *
 * @date 28.09.2026
 * @author zhugengyu
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* the same prologue the driver adapters carry: net80211 expects the
 * ifnet/ifmedia world to exist before ieee80211_var.h */
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
#include <net80211/ieee80211_netbsd.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_proto.h>

#include "port.h"

static struct ieee80211com *wlan_ap_ic(const char *nic)
{
	const struct wlan_port_adapter *a;

	if (nic != NULL) {
		a = wlan_port_adapter_find(nic);
	} else {
		a = wlan_port_adapter_find(wlan_port_active_name());
	}
	if (a == NULL || a->ic == NULL) {
		return NULL;
	}
	return (struct ieee80211com *) a->ic;
}

int
wlan_ap_start(const char *nic, const char *ssid, unsigned ch,
    unsigned bintval)
{
	struct ieee80211com *ic;
	struct ieee80211_channel *chan;
	size_t len;

	if (ssid == NULL || (len = strlen(ssid)) == 0 ||
	    len > IEEE80211_NWID_LEN) {
		return EINVAL;
	}
	if (ch == 0 || ch > IEEE80211_CHAN_MAX ||
	    bintval == 0 || bintval > 1000) {
		return EINVAL;
	}
	ic = wlan_ap_ic(nic);
	if (ic == NULL) {
		return ENODEV;
	}
	if ((ic->ic_caps & IEEE80211_C_HOSTAP) == 0) {
		return EOPNOTSUPP;
	}
	chan = &ic->ic_channels[ch];
	if (chan->ic_flags == 0) {
		return EINVAL;
	}

	/* Radio on: the firmware load and MAC/BB/RF init the adapter's up
	 * hook drives (no-op when already running). */
	if (wlan_port_up_for(nic) != 0) {
		return EIO;
	}

	/* The field writes and the first state arm sit under the serializer
	 * like the scan-start shell does; the deferred legs then run on the
	 * driver worker, which takes the serializer itself. */
	wlan_port_serializer_lock();

	ic->ic_opmode = IEEE80211_M_HOSTAP;
	memset(ic->ic_des_essid, 0, sizeof(ic->ic_des_essid));
	memcpy(ic->ic_des_essid, ssid, len);
	ic->ic_des_esslen = (u_int) len;
	ic->ic_des_chan = chan;
	ic->ic_bintval = (u_int16_t) bintval;
	/* open network: no privacy, no WPA IEs, nothing to drop */
	ic->ic_flags &= ~(IEEE80211_F_WPA1 | IEEE80211_F_WPA2 |
	    IEEE80211_F_PRIVACY | IEEE80211_F_DROPUNENC);
	/* the built-in authenticator authorizes the node at RUN when the
	 * authmode is not 802.1x */
	ic->ic_bss->ni_authmode = IEEE80211_AUTH_OPEN;

	/* INIT leg (idempotent reset), then the SCAN arm: the stack's
	 * SCAN-from-INIT sees HOSTAP + des_chan and runs create_ibss. */
	ieee80211_new_state(ic, IEEE80211_S_INIT, -1);
	ieee80211_new_state(ic, IEEE80211_S_SCAN, 0);

	wlan_port_serializer_unlock();
	return 0;
}

int
wlan_ap_stop(const char *nic)
{
	struct ieee80211com *ic;

	ic = wlan_ap_ic(nic);
	if (ic == NULL) {
		return ENODEV;
	}
	if (ic->ic_opmode != IEEE80211_M_HOSTAP) {
		return ENOENT;
	}

	ieee80211_new_state(ic, IEEE80211_S_INIT, 0);

	/* Back to the station shape so a following scan does not re-enter
	 * create_ibss (SCAN-from-INIT keys on HOSTAP + des_chan).  The
	 * INIT leg queued above does not read these. */
	wlan_port_serializer_lock();
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_des_chan = IEEE80211_CHAN_ANYC;
	ic->ic_des_esslen = 0;
	wlan_port_serializer_unlock();
	return 0;
}

static void
wlan_ap_sta_cb(void *arg, struct ieee80211_node *ni)
{
	(void) arg;
	printf("  sta %02x:%02x:%02x:%02x:%02x:%02x  aid=%u  rssi=%u  "
	    "inact=%u  txrate=%u\n",
	    ni->ni_macaddr[0], ni->ni_macaddr[1], ni->ni_macaddr[2],
	    ni->ni_macaddr[3], ni->ni_macaddr[4], ni->ni_macaddr[5],
	    ni->ni_associd, ni->ni_rssi, ni->ni_inact,
	    ni->ni_txrate < ni->ni_rates.rs_nrates ?
	        (unsigned) (ni->ni_rates.rs_rates[ni->ni_txrate] & IEEE80211_RATE_VAL) : 0);
}

void
wlan_ap_status(const char *nic)
{
	struct ieee80211com *ic;

	ic = wlan_ap_ic(nic);
	if (ic == NULL) {
		printf("wlan: no adapter for ap status\n");
		return;
	}
	printf("ap: opmode=%d state=%s ssid=%.*s ch=%d bintval=%u\n",
	    ic->ic_opmode,
	    ic->ic_state >= 0 && ic->ic_state < IEEE80211_S_MAX ?
	        ieee80211_state_name[ic->ic_state] : "?",
	    (int) ic->ic_des_esslen, (const char *) ic->ic_des_essid,
	    ic->ic_curchan != NULL && ic->ic_curchan->ic_freq != 0 ?
	        ieee80211_chan2ieee(ic, ic->ic_curchan) : 0,
	    ic->ic_bintval);
	if (ic->ic_bss != NULL) {
		printf("ap: bssid=%02x:%02x:%02x:%02x:%02x:%02x capinfo=%04x "
		    "stations=%u\n",
		    ic->ic_bss->ni_bssid[0], ic->ic_bss->ni_bssid[1],
		    ic->ic_bss->ni_bssid[2], ic->ic_bss->ni_bssid[3],
		    ic->ic_bss->ni_bssid[4], ic->ic_bss->ni_bssid[5],
		    ic->ic_bss->ni_capinfo, ic->ic_sta_assoc);
	}
	printf("ap: tim=off (firmware beacon template; beacon_update never "
	    "airs)\n");
	ieee80211_iterate_nodes(&ic->ic_sta, wlan_ap_sta_cb, NULL);
}
