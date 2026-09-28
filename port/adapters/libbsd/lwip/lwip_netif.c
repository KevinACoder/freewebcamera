/*
 * @file
 * @brief lwIP presentation of the net80211 port hooks.
 *
 * Maps the port hooks in port.h onto lwIP netifs - one per attached
 * adapter (a two-NIC image runs AP + station side by side and the loop
 * traffic between them must traverse the air, not the loopback):
 *
 *   - adapter attach (wlan_port_set_attach_notify) creates the netif,
 *     with netif->state pointing at the slot that pairs the two,
 *   - data frames arrive stamped with their adapter, land in pbufs and
 *     are posted through that slot's netif,
 *   - the linkoutput of every slot feeds ITS adapter's xmit - never the
 *     port's active selection,
 *   - source-address routing (LWIP_HOOK_IP4_ROUTE_SRC below) sends a
 *     socket bound to a slot's address out through that slot's netif,
 *     which is what keeps same-subnet loop traffic on the air.
 *
 * Link semantics per role: a station netif follows association (the
 * supplicant bridge reports it through wlan_lwip_assoc_notify_*), an AP
 * netif is administratively raised by `net set` and stays up across
 * station joins and leaves.
 *
 * @date 28.09.2026
 * @author zhugengyu
 */

#include <string.h>
#include <stdio.h>

#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/sys.h"
#include "lwip/ip.h"

#include <port.h>
#include "lwip_netif.h"

#define WLAN_LWIF_MTU 1500
#define WLAN_LWIF_FRAME_MAX (WLAN_LWIF_MTU + 14)

/* one slot per adapter the registry accepted; the pair (slot, adapter)
 * is created at attach and never dissolves (no detach path on this
 * trunk - the SDIO module is soldered, USB dongles re-enumerate only
 * across reboots) */
struct wlan_netif_slot {
	struct netif netif;
	const struct wlan_port_adapter *adapter;
	volatile int link;	/* RX gate + `net` view */
	int dhcp_use;		/* cleared by a static `net set` */
	uint8_t frame[WLAN_LWIF_FRAME_MAX];	/* per-slot tx staging */
};

static struct wlan_netif_slot wlan_slots[WLAN_PORT_NIC_MAX];

static struct wlan_netif_slot *wlan_slot_for(
    const struct wlan_port_adapter *adapter) {
	unsigned i;

	if (adapter == NULL) {
		return NULL;
	}
	for (i = 0; i < WLAN_PORT_NIC_MAX; i++) {
		if (wlan_slots[i].adapter == adapter) {
			return &wlan_slots[i];
		}
	}
	return NULL;
}

/*
 * Bridge drop counters. Every loss below used to be a silent return, so
 * a wedged bulk flow had no trace between net80211 delivery and the
 * tcpip mailbox (M11 TCP-downlink chase). Dumped by wlan_lwip_bridge_dump().
 */
static volatile unsigned long wlan_lwip_rx_posted;
static volatile unsigned long wlan_lwip_rx_assoc_gated;
static volatile unsigned long wlan_lwip_rx_pbuf_fail;
static volatile unsigned long wlan_lwip_rx_take_fail;
static volatile unsigned long wlan_lwip_rx_input_fail;
static volatile unsigned long wlan_lwip_rx_no_slot;

/* ---- rx hooks (driver worker context: copy and return) ---- */

static void wlan_lwip_data_rx(const struct wlan_port_adapter *adapter,
    const uint8_t *frame, size_t len, void *arg) {
	struct wlan_netif_slot *slot = wlan_slot_for(adapter);
	struct pbuf *p;

	(void) arg;
	if (slot == NULL) {
		wlan_lwip_rx_no_slot++;
		return;
	}
	if (!slot->link || len == 0) {
		wlan_lwip_rx_assoc_gated++;
		return;
	}
	p = pbuf_alloc(PBUF_RAW, (u16_t) len, PBUF_RAM);
	if (p == NULL) {
		wlan_lwip_rx_pbuf_fail++;
		return;
	}
	if (pbuf_take(p, frame, (u16_t) len) != ERR_OK) {
		wlan_lwip_rx_take_fail++;
		pbuf_free(p);
		return;
	}
	if (slot->netif.input(p, &slot->netif) != ERR_OK) {
		wlan_lwip_rx_input_fail++;
		pbuf_free(p);
		return;
	}
	wlan_lwip_rx_posted++;
}

void wlan_lwip_bridge_dump(void) {
	unsigned i;

	printf("wlan lwip bridge posted=%lu gated=%lu pbuf_fail=%lu "
	    "take_fail=%lu input_fail=%lu no_slot=%lu\n",
	    wlan_lwip_rx_posted, wlan_lwip_rx_assoc_gated,
	    wlan_lwip_rx_pbuf_fail, wlan_lwip_rx_take_fail,
	    wlan_lwip_rx_input_fail, wlan_lwip_rx_no_slot);
	for (i = 0; i < WLAN_PORT_NIC_MAX; i++) {
		const struct wlan_netif_slot *s = &wlan_slots[i];

		if (s->adapter != NULL) {
			printf("  slot%u %s ip=%s link=%d dhcp=%d\n", i,
			    s->adapter->name,
			    ip4addr_ntoa(netif_ip4_addr(&s->netif)),
			    s->link, s->dhcp_use);
		}
	}
}

static void wlan_lwip_eapol_rx(const struct wlan_port_adapter *adapter,
    const uint8_t src[6], const uint8_t *buf, size_t len, void *arg) {
	(void) adapter;
	(void) src;
	(void) buf;
	(void) len;
	(void) arg;
	/* the supplicant takes this hook over when it starts */
}

/* ---- link state (deferred into the tcpip thread) ---- */

static void wlan_lwip_tcpiplink(void *arg) {
	struct wlan_netif_slot *slot = arg;

	if (slot->link) {
		netif_set_link_up(&slot->netif);
		/* DHCP overwrites a static address when its lease comes
		 * back; only run it when the slot has no static address */
		if (slot->dhcp_use) {
			dhcp_start(&slot->netif);
		}
	} else {
		dhcp_stop(&slot->netif);
		netif_set_link_down(&slot->netif);
	}
}

static void wlan_lwip_dhcp_stop_only(void *arg) {
	dhcp_stop(arg);
}

void wlan_lwip_set_dhcp(int enable) {
	unsigned i;

	for (i = 0; i < WLAN_PORT_NIC_MAX; i++) {
		struct wlan_netif_slot *s = &wlan_slots[i];

		if (s->adapter == NULL) {
			continue;
		}
		s->dhcp_use = enable;
		if (!enable) {
			tcpip_callback(wlan_lwip_dhcp_stop_only, &s->netif);
		}
	}
}

static void wlan_lwip_refresh_hwaddr(struct netif *netif);

/* station association report (the supplicant owns the event hook and
 * forwards); an AP-role adapter ignores it - its netif is raised by
 * `net set` and must survive station joins and leaves */
static void wlan_lwip_assoc_adapter(const struct wlan_port_adapter *adapter,
    int assoc) {
	struct wlan_netif_slot *slot = wlan_slot_for(adapter);

	if (slot == NULL) {
		return;
	}
	slot->link = assoc;
	tcpip_callback(wlan_lwip_refresh_hwaddr, &slot->netif);
	tcpip_callback(wlan_lwip_tcpiplink, slot);
}

void wlan_lwip_assoc_notify_adapter(const struct wlan_port_adapter *adapter,
    int assoc) {
	if (!wlan_port_adapter_is_hostap(adapter)) {
		wlan_lwip_assoc_adapter(adapter, assoc);
	}
}

/* compat: the pre-slot callers ride the port's active adapter */
void wlan_lwip_assoc_notify(int assoc) {
	wlan_lwip_assoc_adapter(
	    wlan_port_adapter_find(wlan_port_active_name()), assoc);
}

/* ---- tx (tcpip thread context) ---- */

static err_t wlan_lwip_linkoutput(struct netif *netif, struct pbuf *p) {
	struct wlan_netif_slot *slot = netif->state;

	if (slot == NULL || slot->adapter == NULL ||
	    slot->adapter->xmit == NULL) {
		return ERR_IF;
	}
	if (p->tot_len > WLAN_LWIF_FRAME_MAX) {
		return ERR_BUF;
	}
	/* per-slot staging: the pbuf chain is not what the driver xmit
	 * wants, and one shared buffer would interleave two adapters'
	 * frames */
	if (pbuf_copy_partial(p, slot->frame, p->tot_len, 0) == 0) {
		return ERR_BUF;
	}

	return (slot->adapter->xmit(slot->frame, p->tot_len) >= 0) ?
	    ERR_OK : ERR_IF;
}

/* pull the MAC once the adapter is attached (tcpip thread context) */
static void wlan_lwip_refresh_hwaddr(struct netif *netif) {
	struct wlan_netif_slot *slot = netif->state;

	if (slot != NULL && slot->adapter != NULL &&
	    slot->adapter->get_hwaddr != NULL &&
	    slot->adapter->get_hwaddr(netif->hwaddr) == 0) {
		netif->hwaddr_len = ETH_HWADDR_LEN;
	}
}

static err_t wlan_lwip_ifinit(struct netif *netif) {
	/* the adapter (and with it the MAC) attaches asynchronously; the
	 * address is refreshed by the link transitions before any frame
	 * goes out */
	netif->name[0] = 'w';
	netif->name[1] = 'l';
	netif->hwaddr_len = ETH_HWADDR_LEN;
	memset(netif->hwaddr, 0, ETH_HWADDR_LEN);
	netif->mtu = WLAN_LWIF_MTU;
	/* ETHARP is required: ethernet_input drops every IP/ARP frame
	 * whose netif lacks the flag */
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
	netif->output = etharp_output;
	netif->linkoutput = wlan_lwip_linkoutput;

	return ERR_OK;
}

/* First address wins the default route (the single-NIC images keep
 * their ntp/tftp behaviour; the two-NIC loop never needs a default).
 * Runs in the tcpip thread from the netif status callback. */
static void wlan_lwip_status(struct netif *netif) {
	if (netif_is_up(netif) && !ip4_addr_isany(netif_ip4_addr(netif)) &&
	    netif_default == NULL) {
		netif_set_default(netif);
	}
}

static void wlan_lwip_adapter_attached(const struct wlan_port_adapter *adapter,
    void *arg) {
	struct wlan_netif_slot *slot = NULL;
	ip4_addr_t zero;
	unsigned i;

	(void) arg;
	if (wlan_slot_for(adapter) != NULL) {
		return; /* replay: already paired */
	}
	for (i = 0; i < WLAN_PORT_NIC_MAX; i++) {
		if (wlan_slots[i].adapter == NULL) {
			slot = &wlan_slots[i];
			break;
		}
	}
	if (slot == NULL) {
		printf("wlan: lwip bridge full, adapter '%s' has no netif\n",
		    adapter->name);
		return;
	}

	slot->adapter = adapter;
	slot->link = 0;
	slot->dhcp_use = 1;
	ip4_addr_set_zero(&zero);
	if (netifapi_netif_add(&slot->netif, &zero, &zero, &zero, slot,
	    wlan_lwip_ifinit, tcpip_input) != ERR_OK) {
		printf("wlan: netif_add failed for '%s'\n", adapter->name);
		slot->adapter = NULL;
		return;
	}
	netif_set_status_callback(&slot->netif, wlan_lwip_status);
	netifapi_netif_set_up(&slot->netif);
	wlan_lwip_refresh_hwaddr(&slot->netif);
	printf("wlan: netif for '%s' ready\n", adapter->name);
}

int wlan_lwip_init(void) {
	wlan_port_set_data_rx(wlan_lwip_data_rx, NULL);
	wlan_port_set_eapol_rx(wlan_lwip_eapol_rx, NULL);
	wlan_port_set_attach_notify(wlan_lwip_adapter_attached, NULL);

	return 0;
}

struct netif *wlan_lwip_get_netif(void) {
	unsigned i;

	for (i = 0; i < WLAN_PORT_NIC_MAX; i++) {
		if (wlan_slots[i].adapter != NULL) {
			return &wlan_slots[i].netif;
		}
	}
	return NULL;
}

/* slot enumeration for the `net` view: idx walks the filled slots in
 * registration order; returns 0 when idx runs past the last one */
int wlan_lwip_view(unsigned idx, const char **name, struct netif **netif,
    int *link) {
	unsigned i, seen = 0;

	for (i = 0; i < WLAN_PORT_NIC_MAX; i++) {
		if (wlan_slots[i].adapter == NULL) {
			continue;
		}
		if (seen++ == idx) {
			*name = wlan_slots[i].adapter->name;
			*netif = &wlan_slots[i].netif;
			*link = wlan_slots[i].link;
			return 0;
		}
	}
	return -1;
}

/* `net set <nic> <ip> <mask> [gw]`: administratively raise a slot -
 * the AP leg's netif comes up this way (address on, DHCP off, link
 * up).  Runs from the shell thread; the netif API carries the locking. */
int wlan_lwip_set_addr(const struct wlan_port_adapter *adapter,
    const ip4_addr_t *ip, const ip4_addr_t *mask, const ip4_addr_t *gw) {
	struct wlan_netif_slot *slot = wlan_slot_for(adapter);

	if (slot == NULL || ip == NULL || mask == NULL) {
		return -1;
	}
	slot->dhcp_use = 0;
	tcpip_callback(wlan_lwip_dhcp_stop_only, &slot->netif);
	netifapi_netif_set_addr(&slot->netif, ip, mask,
	    gw != NULL ? gw : ip);
	wlan_lwip_refresh_hwaddr(&slot->netif);
	slot->link = 1;
	tcpip_callback(wlan_lwip_tcpiplink, slot);
	return 0;
}

int wlan_lwip_ensure(void) {
	return 0; /* the netifs are registered as adapters attach */
}

/* ---- source-address routing ------------------------------------------
 *
 * The board's own AP + station pair sits in ONE subnet, and lwIP picks
 * a netif by destination alone - which would loop .1<->.2 traffic
 * through whichever netif registers first, never through the air.  A
 * socket bound to a slot's address must egress through that slot: this
 * hook (LWIP_HOOK_IP4_ROUTE_SRC, the supported lwIP extension point)
 * answers "which netif owns this source" before ip4_route's
 * destination match runs.  TCP hits it through tcp_route(), UDP and
 * ICMP through their src-aware route calls.
 */
struct netif *lwip_hook_ip4_route_src(const struct ip4_addr *src,
    const struct ip4_addr *dest) {
	struct netif *netif;

	(void) dest;
	if (src == NULL) {
		return NULL;
	}
	NETIF_FOREACH(netif) {
		if (netif_is_up(netif) && netif_is_link_up(netif) &&
		    ip4_addr_cmp(src, netif_ip4_addr(netif))) {
			return netif;
		}
	}
	return NULL;
}
