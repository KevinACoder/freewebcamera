/*
 * @file
 * @brief net shell command: the stack-side view of the wlan netifs,
 * plus the data-bridge drop counters and the static address setter.
 *
 * `net set <nic> <ip> <mask> [gw]` is the AP leg's leg-up (address on,
 * DHCP off, link up) and the station side's escape from DHCP - address
 * changes used to ride DHCP only, which the board loop cannot: there is
 * no DHCP server on this image.
 *
 * @author zhugengyu
 * @date 28.09.2026
 */

#include <string.h>

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "cherrysh_adapter.h"
#include "csh.h"

#include "lwip/lwip_netif.h"
#include <port.h>

static void net_print(chry_shell_t *csh, const char *name,
    struct netif *nif) {
	const ip4_addr_t *ip, *gw, *mask;
	char ipstr[16], gwstr[16], maskstr[16];

	ip = netif_ip4_addr(nif);
	gw = netif_ip4_gw(nif);
	mask = netif_ip4_netmask(nif);
	(void)ip4addr_ntoa_r(ip, ipstr, sizeof(ipstr));
	(void)ip4addr_ntoa_r(gw, gwstr, sizeof(gwstr));
	(void)ip4addr_ntoa_r(mask, maskstr, sizeof(maskstr));

	csh_printf(csh, "net %-10s wl%d "
		   "hwaddr=%02x:%02x:%02x:%02x:%02x:%02x %s%s%s "
		   "ip %s gw %s mask %s\n",
		   name, nif->num,
		   nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
		   nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5],
		   netif_is_up(nif) ? "up" : "down",
		   netif_is_link_up(nif) ? "+link" : "-link",
		   ip4_addr_isany_val(*ip) ? " (no addr)" : "",
		   ipstr, gwstr, maskstr);
}

static int cmd_net(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	unsigned idx;

	if (argc >= 2 && strcmp(argv[1], "bridge") == 0) {
		wlan_lwip_bridge_dump();
		return 0;
	}

	/* net set <nic> <ip> <mask> [gw] - raise a slot statically (the
	 * AP leg's netif, or a station escaping DHCP) */
	if (argc >= 5 && argc <= 6 && strcmp(argv[1], "set") == 0) {
		const struct wlan_port_adapter *adapter =
		    wlan_port_adapter_find(argv[2]);
		ip4_addr_t ip, mask, gw;

		if (adapter == NULL) {
			csh_printf(csh, "net: no adapter '%s' "
				   "(attach it first)\n", argv[2]);
			return 0;
		}
		if (!ip4addr_aton(argv[3], &ip) ||
		    !ip4addr_aton(argv[4], &mask) ||
		    (argc == 6 && !ip4addr_aton(argv[5], &gw))) {
			csh_printf(csh, "net: bad address\n");
			return 0;
		}
		if (wlan_lwip_set_addr(adapter, &ip, &mask,
		    argc == 6 ? &gw : NULL) != 0) {
			csh_printf(csh, "net: set failed (no netif for "
				   "'%s'?)\n", argv[2]);
		}
		return 0;
	}

	for (idx = 0;; idx++) {
		const char *name;
		struct netif *nif;
		int link;

		if (wlan_lwip_view(idx, &name, &nif, &link) != 0) {
			break;
		}
		net_print(csh, name, nif);
	}
	if (idx == 0) {
		csh_printf(csh, "net: no netif registered "
			   "(stack not started, no adapter attached?)\n");
	}
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_net, net, "net [bridge] | net set <nic> <ip> <mask> [gw]",
			  "wlan netif status; set: static address; bridge: rx drop counters");
