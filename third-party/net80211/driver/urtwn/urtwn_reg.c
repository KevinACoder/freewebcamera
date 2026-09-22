/*
 * @file
 * @brief urtwn chip driver registration for the port interface.
 *
 * urtwn_attach() keeps its NetBSD autoconf shape; this adapter plays
 * the role of the USB bus attachment: softc allocation, the device
 * shell, and the usb_attach_arg built from the port device.
 *
 * @date 08.09.2026
 * @author zhugengyu
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/queue.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <sys/sockio.h>
#include <net/if_media.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>
#include <sys/rndsource.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdivar.h>
#include <driver/urtwn/rtwnreg.h>
#include <driver/urtwn/if_urtwnreg.h>
#include <port/port.h>
#include <port/bus/usb/port_usb.h>

#include <driver/urtwn/if_urtwnvar.h>
#include <port/osal/wlan_port_core.h>

/* the verbatim import compiled into this unit so its static glue
 * (CFATTACH_DECL_NEW tables) stays intact */
#include "if_urtwn.c"

static const struct wlan_usb_id urtwn_usb_ids[] = {
	{ 0x0bda, 0x8179 }, /* RTL8188EU */
	{ 0x0bda, 0x0179 }, /* RTL8188EUS */
	{ 0, 0 }
};


struct urtwn_softc *urtwn_reg_softc;

int wlan_urtwn_attach(struct wlan_usb_dev *usb, void *if_priv);

static int wlan_urtwn_attach_bus(void *bus_dev, void *if_priv) {
	return wlan_urtwn_attach((struct wlan_usb_dev *) bus_dev, if_priv);
}

int wlan_urtwn_up(void);
void wlan_urtwn_dump(void);
void wlan_urtwn_scan_dump(void);
int wlan_port_scan_urtwn(const uint8_t *ssid, size_t len);
int wlan_port_xmit_urtwn(const uint8_t *frame, size_t len);
int wlan_port_get_hwaddr_urtwn(uint8_t addr[6]);

static void wlan_ra_hook_start(struct urtwn_softc *sc);

static struct wlan_port_adapter urtwn_adapter = {
	.name = "urtwn",
	.up = wlan_urtwn_up,
	.scan = wlan_port_scan_urtwn,
	.xmit = wlan_port_xmit_urtwn,
	.get_hwaddr = wlan_port_get_hwaddr_urtwn,
	.status_dump = wlan_urtwn_dump,
	.scan_dump = wlan_urtwn_scan_dump,
};

int wlan_urtwn_attach(struct wlan_usb_dev *usb, void *if_priv) {
	struct urtwn_softc *sc;
	struct device *self;
	struct usb_attach_arg uaa;

	(void) if_priv;
	sc = wlan_kmalloc(sizeof(struct urtwn_softc), M_WAITOK | M_ZERO,
	    M_USBDEV);
	if (sc == NULL) {
		return -ENOMEM;
	}
	self = wlan_kmalloc(sizeof(struct device), M_WAITOK | M_ZERO,
	    M_USBDEV);
	if (self == NULL) {
		wlan_kfree(sc, M_USBDEV);
		return -ENOMEM;
	}
	snprintf(self->dv_xname, sizeof(self->dv_xname), "urtwn%u",
	    (unsigned) (wlan_port_if_n - 1));
	self->dv_private = sc;

	memset(&uaa, 0, sizeof(uaa));
	uaa.uaa_vendor = usb->vendor;
	uaa.uaa_product = usb->product;
	uaa.uaa_device = usb->port_priv;

	urtwn_attach(self, self, &uaa);

	if (!sc->sc_dying && ISSET(sc->sc_flags, URTWN_FLAG_ATTACHED)) {
		/* remember the first healthy unit for the scan trigger */
		if (urtwn_reg_softc == NULL) {
			urtwn_reg_softc = sc;
			urtwn_adapter.ic = &sc->sc_ic;
			wlan_port_adapter_register(&urtwn_adapter);
		}
		if (sc == urtwn_reg_softc) {
			wlan_ra_hook_start(sc);
		}
	}
	return 0;
}

struct urtwn_softc *urtwn_reg_softc;

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

static void wlan_dump_key(const char *name, const struct ieee80211_key *key) {
	printf("%s cipher=%s flags=%x index=%u txpn=%llu rxpn=%llu\n",
	    name, key->wk_cipher ? key->wk_cipher->ic_name : "none",
	    key->wk_flags, key->wk_keyix,
	    (unsigned long long)key->wk_keytsc,
	    (unsigned long long)key->wk_keyrsc);
}

void wlan_urtwn_dump(void) {
	struct urtwn_softc *sc = urtwn_reg_softc;
	struct ieee80211com *ic;

	if (sc == NULL) {
		printf("wlan: no device\n");
		return;
	}
	ic = &sc->sc_ic;
	if (ic->ic_bss == NULL) {
		printf("wlan: no BSS\n");
		return;
	}
	printf("wlan mac=%s flags=%x mtu=%u\n",
	    ether_sprintf(ic->ic_myaddr), sc->sc_if.if_flags, sc->sc_if.if_mtu);
	printf("wlan bssid=%s ni_flags=%x caps=%x\n",
	    ether_sprintf(ic->ic_bss->ni_bssid), ic->ic_bss->ni_flags, ic->ic_caps);
	wlan_dump_key("unicast", &ic->ic_bss->ni_ucastkey);
	for (unsigned i = 0; i < IEEE80211_WEP_NKID; i++) {
		printf("group[%u] ", i);
		wlan_dump_key("key", &ic->ic_nw_keys[i]);
	}
	printf("wlan stats tx=%llu txerr=%llu rx=%llu rxerr=%llu auth=%x key=%x\n",
	    (unsigned long long)sc->sc_if.if_data.if_opackets,
	    (unsigned long long)sc->sc_if.if_data.if_oerrors,
	    (unsigned long long)sc->sc_if.if_data.if_ipackets,
	    (unsigned long long)sc->sc_if.if_data.if_ierrors,
	    ic->ic_bss->ni_flags, ic->ic_bss->ni_ucastkey.wk_flags);
	printf("wlan crypto no-key=%u wepfail=%u ccmpmic=%u ccmpreplay=%u unauth=%u\n",
	    ic->ic_stats.is_tx_nodefkey, ic->ic_stats.is_rx_wepfail,
	    ic->ic_stats.is_rx_ccmpmic, ic->ic_stats.is_rx_ccmpreplay,
	    ic->ic_stats.is_rx_unauth);
	printf("wlan ccmpformat=%u\n", ic->ic_stats.is_rx_ccmpformat);
	if (sc->sc_if.if_flags & IFF_RUNNING) {
		printf("urtwn RCR=%08x RXFLTMAP=%04x/%04x/%04x SECCFG=%02x\n",
		    urtwn_read_4(sc, R92C_RCR), urtwn_read_2(sc, R92C_RXFLTMAP0),
		    urtwn_read_2(sc, R92C_RXFLTMAP1), urtwn_read_2(sc, R92C_RXFLTMAP2),
		    urtwn_read_1(sc, R92C_SECCFG));
		printf("urtwn BSSID registers=%08x/%04x\n",
		    urtwn_read_4(sc, R92C_BSSID), urtwn_read_2(sc, R92C_BSSID + 4));
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

/* TX-queue forensics for the mid-stall state.  Each register answers
 * one question: are pages only accumulating (queue jam), is the MAC
 * paused (firmware valve), is the H2C mailbox stuck, or did the DMA
 * engine stop.  Read twice ~1 s apart and compare. */
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

/* --- 88E firmware-maintenance hook ---------------------------------------- *
 *
 * The 88E firmware speaks a different H2C ABI than the 92C one the
 * verbatim driver was written for: MACID_CFG lives at 0x40 (7-byte
 * payload through the 4-byte 88E HMEBOX_EXT window) and RSSI_REPORT
 * at 0x42 (vendor RTL8188E_H2C_CMD_ID), while the driver programs RA
 * only through the dead 92C id 6 - so the firmware rate table is
 * never programmed at all, and under sustained TX its RA engine
 * wedges the chip (bulk OUT stops being served; NetBSD PR 59036 is
 * the same stall on real hardware).
 *
 * This hook sends the vendor association sequence (media status,
 * rate table) once per BSS and feeds RSSI every other tick, on the
 * usb taskq, re-armed by a callout - the same shape as the driver's
 * own calib callout.  "wlan ra 0|1" gates it, "wlan fwfix" fires the
 * association sequence by hand. */

#define WLAN88E_H2C_MEDIA_STATUS	0x01	/* 1B opmode, 1 = connected */
#define WLAN88E_H2C_MACID_CFG		0x40	/* 7B macid,raid,bw,mask32  */
#define WLAN88E_H2C_RSSI_REPORT		0x42	/* 4B macid,0,pwdb,0        */

static callout_t wlan_ra_hook_to;
static struct usb_task wlan_ra_hook_task;
static volatile unsigned wlan_ra_hook_enabled = 1;
static unsigned wlan_ra_hook_sends;
static unsigned wlan_ra_hook_tick;
static uint8_t wlan_ra_hook_bssid[6];

unsigned wlan_ra_hook_get_enabled(void) {
	return wlan_ra_hook_enabled;
}

void wlan_ra_hook_set_enabled(unsigned on) {
	wlan_ra_hook_enabled = on ? 1U : 0U;
}

/* H2C mailbox write in the 88E format: the id goes into the HMEBOX
 * word together with the first three payload bytes, and the 88E's
 * 4-byte HMEBOX_EXT window carries up to four more.  Same box
 * rotation and busy-wait discipline as urtwn_fw_cmd(). */
static int wlan88e_fw_cmd(struct urtwn_softc *sc, uint8_t id,
    const void *buf, int len) {
	const uint8_t *p = buf;
	uint8_t ext[4] = { 0, 0, 0, 0 };
	uint32_t word;
	int ntries;
	int fwcur;

	KASSERT(len <= 7);
	mutex_enter(&sc->sc_fwcmd_mtx);
	fwcur = sc->fwcur;
	sc->fwcur = (sc->fwcur + 1) % R92C_H2C_NBOX;

	/* wait for the box to drain */
	for (ntries = 0; ntries < 100; ntries++) {
		if (!(urtwn_read_1(sc, R92C_HMETFR) & (1 << fwcur)))
			break;
		urtwn_delay_ms(sc, 2);
	}
	if (ntries == 100) {
		mutex_exit(&sc->sc_fwcmd_mtx);
		return ETIMEDOUT;
	}

	if (len > 3) {
		int i;

		for (i = 3; i < len && i < 7; i++)
			ext[i - 3] = p[i];
		/* ext window first: the HMEBOX word write triggers */
		urtwn_write_region(sc, R88E_HMEBOX_EXT(fwcur), ext, 4);
	}
	word = (uint32_t) id |
	    ((uint32_t) (len > 0 ? p[0] : 0) << 8) |
	    ((uint32_t) (len > 1 ? p[1] : 0) << 16) |
	    ((uint32_t) (len > 2 ? p[2] : 0) << 24);
	urtwn_write_4(sc, R92C_HMEBOX(fwcur), word);
	mutex_exit(&sc->sc_fwcmd_mtx);
	return 0;
}

/* rate bitmaps over ni_rates with the standard hw-rate map (same
 * mapping urtwn_ra_init uses) */
static void wlan88e_rates_mask(struct ieee80211_node *ni,
    uint32_t *rates, uint32_t *basicrates) {
	static const uint8_t map[] = {
		2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
	};
	struct ieee80211_rateset *rs = &ni->ni_rates;
	uint32_t i, j;

	*rates = 1;
	*basicrates = 1;
	for (i = 0; i < rs->rs_nrates; i++) {
		for (j = 0; j < sizeof(map) / sizeof(map[0]); j++) {
			if ((rs->rs_rates[i] & IEEE80211_RATE_VAL) == map[j])
				break;
		}
		if (j < sizeof(map) / sizeof(map[0])) {
			*rates |= 1U << j;
			if (rs->rs_rates[i] & IEEE80211_RATE_BASIC)
				*basicrates |= 1U << j;
		}
	}
}

/* program one firmware RA entry: 7-byte MACID_CFG */
static int wlan88e_macid_cfg(struct urtwn_softc *sc, uint8_t macid,
    uint8_t raid_flags, uint32_t rates) {
	uint8_t msg[7];

	memset(msg, 0, sizeof(msg));
	msg[0] = macid;
	msg[1] = raid_flags;
	msg[2] = 0;		/* 20 MHz */
	msg[3] = (uint8_t) (rates & 0xff);
	msg[4] = (uint8_t) ((rates >> 8) & 0xff);
	msg[5] = (uint8_t) ((rates >> 16) & 0xff);
	msg[6] = (uint8_t) ((rates >> 24) & 0xff);
	return wlan88e_fw_cmd(sc, WLAN88E_H2C_MACID_CFG, msg, 7);
}

static void wlan_ra_hook_arm(struct urtwn_softc *sc);

static void wlan_ra_hook_task_fn(void *arg) {
	struct urtwn_softc *sc = arg;
	struct ieee80211com *ic;
	uint8_t msg[7];
	uint32_t rates, basicrates;
	int error = 0;

	if (sc == NULL || sc->sc_dying || !wlan_ra_hook_enabled) {
		return;
	}
	ic = &sc->sc_ic;
	if (ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL) {
		/* left RUN: send again on the next association */
		wlan_ra_hook_sends = 0;
		wlan_ra_hook_arm(sc);
		return;
	}
	wlan_ra_hook_tick++;

	if (wlan_ra_hook_sends != 0) {
		/* association programmed: feed RSSI every other tick */
		if ((wlan_ra_hook_tick & 1) == 0 && sc->avg_pwdb >= 0) {
			memset(msg, 0, sizeof(msg));
			msg[0] = RTWN_MACID_BSS;
			msg[2] = (uint8_t) sc->avg_pwdb;
			mutex_enter(&sc->sc_write_mtx);
			error = wlan88e_fw_cmd(sc, WLAN88E_H2C_RSSI_REPORT,
			    msg, 4);
			mutex_exit(&sc->sc_write_mtx);
			if (error != 0) {
				printf("[wlan] 88e fw rssi report: busy\n");
			}
		}
		wlan_ra_hook_arm(sc);
		return;
	}
	if (memcmp(ic->ic_bss->ni_bssid, wlan_ra_hook_bssid,
	    sizeof(wlan_ra_hook_bssid)) == 0 && wlan_ra_hook_tick < 60) {
		/* same BSS and nothing new: only retry for a minute */
		wlan_ra_hook_arm(sc);
		return;
	}

	mutex_enter(&sc->sc_write_mtx);

	/* media status: connected */
	memset(msg, 0, sizeof(msg));
	msg[0] = 0x01;
	error = wlan88e_fw_cmd(sc, WLAN88E_H2C_MEDIA_STATUS, msg, 1);

	/* rate tables: the broadcast entry first (the driver tags every
	 * bc/mc frame with macid 4 - without a firmware RA entry the
	 * firmware cannot send ARP/DHCP at all), then the BSS station */
	wlan88e_rates_mask(ic->ic_bss, &rates, &basicrates);
	if (error == 0) {
		error = wlan88e_macid_cfg(sc, RTWN_MACID_BC,
		    R92C_RAID_11BG, basicrates);
	}
	if (error == 0) {
		error = wlan88e_macid_cfg(sc, RTWN_MACID_BSS,
		    R92C_RAID_11BG, rates);
	}

	/* initial rate hints (register-level, chip side), as ra_init */
	if (error == 0) {
		urtwn_write_1(sc, R92C_INIDATA_RATE_SEL(RTWN_MACID_BC),
		    0);		/* CCK 1 Mbps */
		urtwn_write_1(sc, R92C_INIDATA_RATE_SEL(RTWN_MACID_BSS),
		    4);		/* OFDM 6 Mbps */
	}

	mutex_exit(&sc->sc_write_mtx);

	if (error != 0) {
		/* the firmware mailbox does not drain during association
		 * churn; nothing was recorded, the next tick retries */
		printf("[wlan] 88e fw maintenance: h2c not ready, retry\n");
		wlan_ra_hook_arm(sc);
		return;
	}

	memcpy(wlan_ra_hook_bssid, ic->ic_bss->ni_bssid,
	    sizeof(wlan_ra_hook_bssid));
	wlan_ra_hook_sends++;
	printf("[wlan] 88e fw maintenance: media_status + macid_cfg "
	    "(rates=%08x basic=%08x)\n", rates, basicrates);
	wlan_ra_hook_arm(sc);
}

static void wlan_ra_hook_timer(void *arg) {
	struct urtwn_softc *sc = arg;

	if (sc == NULL || sc->sc_dying || !wlan_ra_hook_enabled) {
		return;
	}
	usb_add_task(sc->sc_udev, &wlan_ra_hook_task, USB_TASKQ_DRIVER);
}

static void wlan_ra_hook_arm(struct urtwn_softc *sc) {
	if (sc == NULL || sc->sc_dying || !wlan_ra_hook_enabled) {
		return;
	}
	callout_schedule(&wlan_ra_hook_to, hz);
}

void wlan_ra_hook_force(void) {
	struct urtwn_softc *sc = urtwn_reg_softc;

	if (sc == NULL || sc->sc_dying || !wlan_ra_hook_enabled) {
		printf("wlan: ra hook not armed (device down or disabled)\n");
		return;
	}
	wlan_ra_hook_sends = 0;
	usb_add_task(sc->sc_udev, &wlan_ra_hook_task, USB_TASKQ_DRIVER);
}

void wlan_ra_hook_kick(void) {
	struct urtwn_softc *sc = urtwn_reg_softc;

	wlan_ra_hook_arm(sc);
}

static void wlan_ra_hook_start(struct urtwn_softc *sc) {
	static int started;

	if (started) {
		return;
	}
	started = 1;
	callout_init(&wlan_ra_hook_to, 0);
	callout_setfunc(&wlan_ra_hook_to, wlan_ra_hook_timer, sc);
	usb_init_task(&wlan_ra_hook_task, wlan_ra_hook_task_fn, sc, 0);
	callout_schedule(&wlan_ra_hook_to, hz);
}

void wlan_urtwn_detach(void *priv) {
	struct wlan_usb_dev *usb = priv;
	(void) usb;
	/* detach goes through urtwn_detach with the stored device shell */
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

const struct wlan_chip_driver urtwn_driver = {
	.name = "urtwn",
	.bus = WLAN_BUS_USB,
	.usb_ids = urtwn_usb_ids,
	.attach = wlan_urtwn_attach_bus,
	.detach = wlan_urtwn_detach,
	.stop = NULL,
};
