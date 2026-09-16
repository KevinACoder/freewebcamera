/*
 * @file   lwip_adapter.c
 * @brief  lwIP bring-up for this board: the one place where the stack, the two
 *         MAC instances and the two PHY instances are named together.
 *
 * This is the adapter's registration point (modularity check K3): the port
 * table below is the only spot in the project that mentions Driver_ETH_MAC0/1
 * and Driver_ETH_PHY0/1. Swapping the MAC implementation, or the stack, means
 * editing this file and nothing else - app/ only ever calls net_start().
 *
 * Bring-up order per port, and why:
 *
 *   1. MAC Initialize + PowerControl(FULL)
 *        - clocks, pin iomux, RGMII delays and the PHY hard reset pulse,
 *          then the DMA rings. The pulse re-latches the PHY's strapped RGMII
 *          TX delay, which is what step 2 has to undo.
 *   2. PHY Initialize (over that MAC's MDIO) + PowerControl(FULL)
 *        - find the PHY, clear the strapped delay, soft reset the PHY.
 *   3. PHY SetInterface + SetMode(AUTO_NEGOTIATE)
 *        - advertise gigabit with the 10/100 fallback and wait for the link.
 *   4. MAC Control(CONFIGURE) with what the PHY negotiated, then
 *      Control(TX/RX) to start the channel.
 *   5. netif_add / set_up / set_link_up, then the receive thread.
 *
 * Addresses are static for now, which is what the milestone asks for ("static
 * IP first, ping it, then move on"): the reliable port carries the default
 * route and the gateway, the second port is plain on the same subnet. Both
 * ports on one subnet means lwIP's outbound route is the first matching netif
 * regardless of which port a packet arrived on - fine for ICMP echo (the reply
 * goes back out the netif it arrived on) and the reason the acceptance runs
 * one port at a time (see docs/ROADMAP.md). DHCP is compiled in and off by
 * default; the shell can turn it on per port.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <string.h>

#include "Driver_ETH_MAC.h"
#include "Driver_ETH_PHY.h"
#include "board.h"
#include "cmsis_os2.h"
#include "lwip/dhcp.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netifapi.h"
#include "lwip/tcpip.h"
#include "net.h"

#include "eth_port.h"

/* The MAC and PHY instances, through the interface headers only. */
extern ARM_DRIVER_ETH_MAC Driver_ETH_MAC0;
extern ARM_DRIVER_ETH_MAC Driver_ETH_MAC1;
extern ARM_DRIVER_ETH_PHY Driver_ETH_PHY0;
extern ARM_DRIVER_ETH_PHY Driver_ETH_PHY1;

/* Implemented in lwip_diag.c: tells the diagnostic sink that the console
 * exists, so lwIP assertions can be printed. */
void lwip_diag_set_console_ready(void);

/* --- the port table (the single registration point) ----------------------- */

struct eth_port eth_ports[ETH_PORT_COUNT] = {
	[0] = {
		.mac   = &Driver_ETH_MAC0,
		.phy   = &Driver_ETH_PHY0,
		.index = 0,
		.label = "gmac0",
	},
	[1] = {
		.mac   = &Driver_ETH_MAC1,
		.phy   = &Driver_ETH_PHY1,
		.index = 1,
		.label = "gmac1",
	},
};

struct eth_port *eth_port_get(unsigned int index)
{
	return (index < ETH_PORT_COUNT) ? &eth_ports[index] : NULL;
}

/* Static addressing, per port. The lab's host is 192.168.0.18 and the gateway
 * 192.168.0.1; gmac1 is the port the lab's TFTP/NFS traffic has always used
 * and carries the default route, gmac0 is the second port (historically the
 * weaker one - KI-003). */
static const struct eth_port_addr {
	const char *ip;
	const char *mask;
	const char *gw;		/* NULL: no gateway on this port */
	int         default_route;
} port_addrs[ETH_PORT_COUNT] = {
	[0] = { .ip = "192.168.0.201", .mask = "255.255.255.0", .gw = NULL,
		.default_route = 0 },
	[1] = { .ip = "192.168.0.200", .mask = "255.255.255.0", .gw = "192.168.0.1",
		.default_route = 1 },
};

/* --- event callbacks ------------------------------------------------------ */

/* One per port: the CMSIS event callback has no context argument. The event
 * body runs in interrupt context. */
static void eth0_event(uint32_t event)
{
	ethernetif_event(&eth_ports[0], event);
}

static void eth1_event(uint32_t event)
{
	ethernetif_event(&eth_ports[1], event);
}

static ARM_ETH_MAC_SignalEvent_t port_event_cb[ETH_PORT_COUNT] = {
	eth0_event, eth1_event,
};

/* --- helpers -------------------------------------------------------------- */

static int port_addr_parse(const struct eth_port_addr *cfg, ip4_addr_t *ip,
			   ip4_addr_t *mask, ip4_addr_t *gw)
{
	if ((ip4addr_aton(cfg->ip, ip) == 0) || (ip4addr_aton(cfg->mask, mask) == 0)) {
		return -1;
	}
	if (cfg->gw != NULL) {
		if (ip4addr_aton(cfg->gw, gw) == 0) {
			return -1;
		}
	} else {
		ip4_addr_set_zero(gw);
	}

	return 0;
}

/* Program the MAC with the PHY's negotiated speed and duplex. Called at
 * bring-up and again whenever the link comes back at a different rate. */
static void eth_port_apply_link(struct eth_port *port)
{
	ARM_ETH_LINK_INFO info;
	uint32_t arg = ARM_ETH_MAC_ADDRESS_BROADCAST | ARM_ETH_MAC_ADDRESS_MULTICAST;

	/* The link state first, and not the speed: ARM_ETH_SPEED_10M is 0, so a
	 * link-info structure filled in while the link is down (all zeroes)
	 * reads as a perfectly plausible "10 Mbps" and would be programmed into
	 * the MAC. */
	if (port->phy->GetLinkState() != ARM_ETH_LINK_UP) {
		return;
	}
	info = port->phy->GetLinkInfo();

	switch (info.speed) {
	case ARM_ETH_SPEED_1G:
		arg |= ARM_ETH_MAC_SPEED_1G;
		break;
	case ARM_ETH_SPEED_100M:
		arg |= ARM_ETH_MAC_SPEED_100M;
		break;
	case ARM_ETH_SPEED_10M:
		arg |= ARM_ETH_MAC_SPEED_10M;
		break;
	default:
		/* Not resolved: leave the MAC as it is rather than program a
		 * speed nobody reported. */
		return;
	}
	arg |= (info.duplex == ARM_ETH_DUPLEX_FULL) ? ARM_ETH_MAC_DUPLEX_FULL
						    : ARM_ETH_MAC_DUPLEX_HALF;
	(void)port->mac->Control(ARM_ETH_MAC_CONFIGURE, arg);
}

static void eth_port_report_link(struct eth_port *port)
{
	const ip4_addr_t *ip = netif_ip4_addr(&port->netif);
	char addr[16];

	(void)ip4addr_ntoa_r(ip, addr, sizeof(addr));

	if (port->phy->GetLinkState() != ARM_ETH_LINK_UP) {
		board_log("net: %s link down addr %s\n", port->label, addr);
		return;
	}

	{
		ARM_ETH_LINK_INFO info = port->phy->GetLinkInfo();

		board_log("net: %s link up %s Mbps %s duplex addr %s\n", port->label,
			  (info.speed == ARM_ETH_SPEED_1G) ? "1000"
			  : (info.speed == ARM_ETH_SPEED_100M) ? "100" : "10",
			  (info.duplex == ARM_ETH_DUPLEX_FULL) ? "full" : "half", addr);
	}
}

/* --- per-port bring-up ---------------------------------------------------- */

static int eth_port_bring_up(unsigned int index)
{
	struct eth_port *port = eth_port_get(index);
	const struct eth_port_addr *cfg = &port_addrs[index];
	osThreadAttr_t attr = { .name = "ethrx", .stack_size = 2048,
				.priority = osPriorityNormal };
	ip4_addr_t ip, mask, gw;

	if (port_addr_parse(cfg, &ip, &mask, &gw) != 0) {
		board_log("net: %s has an unparsable address\n", port->label);
		return -1;
	}

	/* 1. MAC: clocks, iomux, RGMII delays, PHY reset pulse, DMA rings. */
	if (port->mac->Initialize(port_event_cb[index]) != ARM_DRIVER_OK) {
		board_log("net: %s mac initialize failed\n", port->label);
		return -1;
	}
	if (port->mac->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
		board_log("net: %s mac power-up failed\n", port->label);
		return -1;
	}

	/* 2. PHY over this MAC's management interface. */
	if (port->phy->Initialize(port->mac->PHY_Read, port->mac->PHY_Write) != ARM_DRIVER_OK) {
		board_log("net: %s phy initialize failed\n", port->label);
		return -1;
	}
	if (port->phy->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
		/* No PHY answering: the port stays down, the stack still
		 * comes up on the other one, and `net` says so. */
		board_log("net: %s phy not responding, port left down\n", port->label);
		return -1;
	}

	/* 3. Interface and autonegotiation. */
	(void)port->phy->SetInterface(ARM_ETH_INTERFACE_RGMII);
	if (port->phy->SetMode(ARM_ETH_PHY_AUTO_NEGOTIATE) != ARM_DRIVER_OK) {
		/* No link within the wait, which is not fatal: the link monitor
		 * polls the PHY and picks the port up when the partner is
		 * ready. Measured on this board, the RTL8211F occasionally
		 * needs longer than the port's autoneg wait at a cold start. */
		board_log("net: %s no link yet, left to the link monitor\n", port->label);
	}

	/* 4. MAC rate from what the PHY reported, engines on. */
	eth_port_apply_link(port);
	(void)port->mac->Control(ARM_ETH_MAC_CONTROL_TX, 1);
	(void)port->mac->Control(ARM_ETH_MAC_CONTROL_RX, 1);

	/* 5. The netif, in the stack's own thread (netifapi_* blocks until the
	 * tcpip thread has done the work). */
	if (netifapi_netif_add(&port->netif, &ip, &mask, &gw, port,
			       ethernetif_init, tcpip_input) != ERR_OK) {
		board_log("net: %s netif_add failed\n", port->label);
		return -1;
	}
	if (cfg->default_route) {
		(void)netifapi_netif_set_default(&port->netif);
	}
	(void)netifapi_netif_set_up(&port->netif);
	if (port->phy->GetLinkState() == ARM_ETH_LINK_UP) {
		(void)netifapi_netif_set_link_up(&port->netif);
	}

	/* The receive thread last: frames that arrived earlier are staged in
	 * the driver and drained on its first wakeup. */
	port->rx_thread = osThreadNew(ethernetif_rx_thread, port, &attr);
	if (port->rx_thread == NULL) {
		board_log("net: %s rx thread failed\n", port->label);
		return -1;
	}

	eth_port_report_link(port);

	return 0;
}

/* --- link monitor --------------------------------------------------------- */

/* Link state changes are watched rather than interrupt-driven: the PHY has no
 * interrupt line wired on this board, and polling it once a second costs two
 * MDIO reads. On a change the MAC is reprogrammed and lwIP told. */
static void eth_link_thread(void *argument)
{
	(void)argument;

	for (;;) {
		unsigned int i;

		osDelay(1000);

		for (i = 0; i < ETH_PORT_COUNT; i++) {
			struct eth_port *port = &eth_ports[i];
			ARM_ETH_LINK_STATE state;

			if (port->rx_thread == NULL) {
				continue;	/* port never came up */
			}
			state = port->phy->GetLinkState();
			if (state == ARM_ETH_LINK_UP) {
				if (!netif_is_link_up(&port->netif)) {
					eth_port_apply_link(port);
					(void)netifapi_netif_set_link_up(&port->netif);
					eth_port_report_link(port);
				}
			} else if (netif_is_link_up(&port->netif)) {
				(void)netifapi_netif_set_link_down(&port->netif);
				board_log("net: %s link down\n", port->label);
			}
		}
	}
}

/* --- entry point ---------------------------------------------------------- */

int net_start(void)
{
	static int started;
	osThreadAttr_t attr = { .name = "ethlink", .stack_size = 1536,
				.priority = osPriorityLow };
	unsigned int i;
	unsigned int ports_up = 0;

	if (started) {
		return 0;
	}
	started = 1;

	lwip_diag_set_console_ready();
	board_log("net: lwip %s, %u ports\n", LWIP_VERSION_STRING, ETH_PORT_COUNT);

	/* Starts the tcpip thread; every netif operation below goes through
	 * netifapi_* so it happens in that thread, not this one. */
	tcpip_init(NULL, NULL);

	for (i = 0; i < ETH_PORT_COUNT; i++) {
		if (eth_port_bring_up(i) == 0) {
			ports_up++;
		}
	}

	if (osThreadNew(eth_link_thread, NULL, &attr) == NULL) {
		board_log("net: link thread failed\n");
	}

	board_log("net: up (%u of %u ports)\n", ports_up, ETH_PORT_COUNT);

	/* The stack is up either way: a port without a link is a state the
	 * system is expected to run in, not a failed start. */
	return 0;
}