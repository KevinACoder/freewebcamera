/*
 * @file
 * @brief net shell command: the stack-side view of the wlan netif, plus the
 * data-bridge drop counters.
 *
 * Display only - address changes ride DHCP (or the supplicant lane later);
 * this is where the board ladder reads back what DHCP actually bound and
 * where a wedged flow gets bisected when the radio looks fine.
 *
 * @author zhugengyu
 * @date 25.09.2026
 */

#include <string.h>

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "cherrysh_adapter.h"
#include "csh.h"

#include "lwip/lwip_netif.h"

static int cmd_net(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	struct netif *nif = wlan_lwip_get_netif();
	const ip4_addr_t *ip, *gw, *mask;
	char ipstr[16], gwstr[16], maskstr[16];

	if (argc >= 2 && strcmp(argv[1], "bridge") == 0) {
		wlan_lwip_bridge_dump();
		return 0;
	}

	if (nif == NULL) {
		csh_printf(csh, "net: no netif registered "
			   "(stack not started?)\n");
		return 0;
	}

	ip = netif_ip4_addr(nif);
	gw = netif_ip4_gw(nif);
	mask = netif_ip4_netmask(nif);
	(void)ip4addr_ntoa_r(ip, ipstr, sizeof(ipstr));
	(void)ip4addr_ntoa_r(gw, gwstr, sizeof(gwstr));
	(void)ip4addr_ntoa_r(mask, maskstr, sizeof(maskstr));

	csh_printf(csh, "net wl%d hwaddr=%02x:%02x:%02x:%02x:%02x:%02x "
		   "mtu=%d %s%s%s ip %s gw %s mask %s\n",
		   nif->num,
		   nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
		   nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5],
		   nif->mtu,
		   netif_is_up(nif) ? "up" : "down",
		   netif_is_link_up(nif) ? "+link" : "-link",
		   ip4_addr_isany_val(*ip) ? " (no addr)" : "",
		   ipstr, gwstr, maskstr);

	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_net, net, "net [bridge]",
			  "wlan netif status; bridge: rx drop counters");
