/*
 * @file
 * @brief lwIP presentation of the net80211 port hooks.
 */

#ifndef LWIP_NETIF_H_
#define LWIP_NETIF_H_

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

struct wlan_port_adapter;

/* Register the rx hooks and the attach notification (call once after
 * tcpip_init()); the netifs themselves appear as adapters attach. */
int wlan_lwip_init(void);

/* The first registered netif (compat view for the single-NIC images). */
struct netif *wlan_lwip_get_netif(void);

/* For the supplicant lane: the supplicant owns the port event hook,
 * so it reports association state here. Drives the MAC refresh, the
 * link state and DHCP on the tcpip thread - station-role adapters
 * only; an AP-role adapter's netif is raised by net set instead. */
void wlan_lwip_assoc_notify(int assoc);
void wlan_lwip_assoc_notify_adapter(const struct wlan_port_adapter *adapter,
    int assoc);

/* No-op on this port (the netifs exist from adapter attach). */
int wlan_lwip_ensure(void);

/* Administratively raise a slot: static address, DHCP off, link up -
 * the AP leg's netif comes up this way. Returns 0 on success. */
int wlan_lwip_set_addr(const struct wlan_port_adapter *adapter,
    const ip4_addr_t *ip, const ip4_addr_t *mask, const ip4_addr_t *gw);

/* disable the DHCP started on ASSOC so a static address sticks */
void wlan_lwip_set_dhcp(int enable);

/* Slot enumeration for the `net` view (name = adapter name). */
int wlan_lwip_view(unsigned idx, const char **name, struct netif **netif,
    int *link);

/* Drop/post counters of the data_rx bridge plus the slot table:
 * posted frames and each silent-loss point (link gate, pbuf_alloc,
 * pbuf_take, mbox input, unknown adapter). Snapshot twice and diff
 * across a wedged flow. */
void wlan_lwip_bridge_dump(void);

#endif /* LWIP_NETIF_H_ */
