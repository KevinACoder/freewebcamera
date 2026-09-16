/*
 * @file   net_cmds.c
 * @brief  Shell commands for the Ethernet ports: the operational view the
 *         acceptance runs are written against.
 *
 *   net                     per-port state: link, speed, address, counters
 *   net up|down <n>         take a port's interface up or down
 *   net ip <n> <a.b.c.d>    static address (mask and gateway keep their value)
 *   net dhcp <n> on|off     DHCP on a port (off by default: this milestone
 *                           uses static addresses)
 *
 * `net down` is the tool for the same-subnet problem: with both ports on one
 * subnet lwIP's route to the host is the first matching netif, so validating a
 * single port means taking the other one down first (docs/ROADMAP.md says the
 * same thing about the emBox port before this one).
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "cherrysh_adapter.h"
#include "csh.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netifapi.h"

#include "eth_port.h"

/* --- small formatting helpers --------------------------------------------- */

static void addr_str(const ip4_addr_t *addr, char *out, unsigned int size)
{
	(void)ip4addr_ntoa_r(addr, out, size);
}

static const char *speed_str(const ARM_ETH_LINK_INFO *info)
{
	if (info->speed == ARM_ETH_SPEED_1G) {
		return "1000";
	}
	if (info->speed == ARM_ETH_SPEED_100M) {
		return "100";
	}
	if (info->speed == ARM_ETH_SPEED_10M) {
		return "10";
	}

	return "??";
}

static void port_print(chry_shell_t *csh, struct eth_port *port)
{
	char ip[16];
	char mask[16];
	char gw[16];
	ARM_ETH_MAC_ADDR mac;
	ARM_ETH_LINK_INFO info;

	if (port->mac->GetMacAddress(&mac) != ARM_DRIVER_OK) {
		memset(&mac, 0, sizeof(mac));
	}
	addr_str(netif_ip4_addr(&port->netif), ip, sizeof(ip));
	addr_str(netif_ip4_netmask(&port->netif), mask, sizeof(mask));
	addr_str(netif_ip4_gw(&port->netif), gw, sizeof(gw));

	if (port->rx_thread == NULL) {
		csh_printf(csh, "%s %-4s (not started)\n", port->label, port->netif.name);
		return;
	}

	if (netif_is_link_up(&port->netif)) {
		info = port->phy->GetLinkInfo();
		csh_printf(csh, "%s %-4s link up %s/%s", port->label, port->netif.name,
			   speed_str(&info),
			   (info.duplex == ARM_ETH_DUPLEX_FULL) ? "full" : "half");
	} else {
		csh_printf(csh, "%s %-4s link down", port->label, port->netif.name);
	}

	csh_printf(csh, "%s  ip %s/%s", netif_is_up(&port->netif) ? "" : " (down)",
		   ip, mask);
	if (!ip4_addr_isany_val(*netif_ip4_gw(&port->netif))) {
		csh_printf(csh, " gw %s", gw);
	}
	csh_printf(csh, "  mac %02x:%02x:%02x:%02x:%02x:%02x\n",
		   mac.b[0], mac.b[1], mac.b[2], mac.b[3], mac.b[4], mac.b[5]);
	csh_printf(csh, "      rx %u tx %u ev %u dropped %u\n",
		   port->rx_frames, port->tx_frames, port->rx_events, port->err_drops);
}

/* --- argument helpers ----------------------------------------------------- */

/* <n> is the port index, which is also the netif number (e0/e1). */
static struct eth_port *port_from_arg(const char *arg)
{
	if ((arg == NULL) || (arg[0] == '\0') || (arg[1] != '\0')) {
		return NULL;
	}

	return eth_port_get((unsigned int)(arg[0] - '0'));
}

/* --- commands ------------------------------------------------------------- */

static int cmd_net(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	struct eth_port *port;
	unsigned int i;

	if (argc >= 2) {
		const char *op = argv[1];

		if ((strcmp(op, "up") == 0) || (strcmp(op, "down") == 0)) {
			port = (argc >= 3) ? port_from_arg(argv[2]) : &eth_ports[1];
			if (port == NULL) {
				csh_printf(csh, "net: port must be 0 or 1\r\n");
				return -1;
			}
			if (strcmp(op, "up") == 0) {
				(void)netifapi_netif_set_up(&port->netif);
				/* The driver keeps receiving either way; this is
				 * lwIP's view of the interface. */
				csh_printf(csh, "net: %s up\r\n", port->label);
			} else {
				(void)netifapi_netif_set_down(&port->netif);
				csh_printf(csh, "net: %s down\r\n", port->label);
			}
			return 0;
		}

		if (strcmp(op, "ip") == 0) {
			ip4_addr_t ip;
			ip4_addr_t mask;

			if ((argc < 4) || (NULL == (port = port_from_arg(argv[2])))) {
				csh_printf(csh, "usage: net ip <0|1> <a.b.c.d>\r\n");
				return -1;
			}
			if (ip4addr_aton(argv[3], &ip) == 0) {
				csh_printf(csh, "net: bad address '%s'\r\n", argv[3]);
				return -1;
			}
			ip4_addr_copy(mask, *netif_ip4_netmask(&port->netif));
			(void)netifapi_netif_set_addr(&port->netif, &ip, &mask,
						      netif_ip4_gw(&port->netif));
			csh_printf(csh, "net: %s address set\r\n", port->label);
			return 0;
		}

		if (strcmp(op, "dhcp") == 0) {
			if ((argc < 4) || (NULL == (port = port_from_arg(argv[2])))) {
				csh_printf(csh, "usage: net dhcp <0|1> <on|off>\r\n");
				return -1;
			}
			if (strcmp(argv[3], "on") == 0) {
				csh_printf(csh, "net: %s dhcp %s\r\n", port->label,
					   (netifapi_dhcp_start(&port->netif) == ERR_OK)
					   ? "started" : "failed");
			} else {
				(void)netifapi_dhcp_release_and_stop(&port->netif);
				csh_printf(csh, "net: %s dhcp stopped\r\n", port->label);
			}
			return 0;
		}

		csh_printf(csh, "usage: net [up|down|ip|dhcp] ...\r\n");
		return -1;
	}

	for (i = 0; i < ETH_PORT_COUNT; i++) {
		port_print(csh, &eth_ports[i]);
	}

	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_net, net, "net [up|down|ip|dhcp] <n> [args]",
			  "show or change the ethernet ports");