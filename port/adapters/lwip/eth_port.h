/*
 * @file   eth_port.h
 * @brief  One Ethernet port as this adapter sees it: a CMSIS MAC, a CMSIS PHY
 *         and the lwIP netif that fronts them.
 *
 * Internal to port/adapters/lwip/ - not an interface. The port table itself
 * lives in lwip_adapter.c, which is the single place where implementations are
 * named (modularity check K3); ethernetif.c only works with ports it is handed.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_ETH_PORT_H
#define FREEWEBCAMERA_ETH_PORT_H

#include <stdint.h>

#include "Driver_ETH_MAC.h"
#include "Driver_ETH_PHY.h"
#include "cmsis_os2.h"
#include "lwip/netif.h"

#define ETH_PORT_COUNT	2

/* Thread flag raised from the MAC's interrupt when a frame (or a batch of
 * them) has been staged for this port. */
#define ETH_PORT_RX_FLAG	0x1u

/* Largest frame the MAC driver will transmit (it refuses longer ones). */
#define ETH_PORT_MAX_FRAME	1600

struct eth_port {
	/* The implementations, named once in lwip_adapter.c. */
	const ARM_DRIVER_ETH_MAC *mac;
	const ARM_DRIVER_ETH_PHY *phy;

	unsigned int index;	/* 0..ETH_PORT_COUNT-1, also the netif number */
	const char  *label;	/* "gmac0"/"gmac1": what the board calls the port */
	struct netif netif;

	osThreadId_t rx_thread;

	/* Counters the shell reports. The adapter's view: frames that reached
	 * lwIP and frames lwIP handed down, not register-level statistics (the
	 * driver logs what it has to drop). */
	uint32_t rx_frames;
	uint32_t rx_events;
	uint32_t tx_frames;
	uint32_t err_drops;

	/* Transmit linearization: the MAC takes one contiguous frame, lwIP
	 * hands over a pbuf chain. Only the stack's own thread transmits, so
	 * one buffer per port is enough. */
	uint8_t tx_buf[ETH_PORT_MAX_FRAME] __attribute__((aligned(16)));
};

extern struct eth_port eth_ports[ETH_PORT_COUNT];

struct eth_port *eth_port_get(unsigned int index);

/* ethernetif.c: the lwIP-facing half of a port. */
err_t ethernetif_init(struct netif *netif);
void  ethernetif_input(struct eth_port *port);
void  ethernetif_rx_thread(void *argument);
void  ethernetif_event(struct eth_port *port, uint32_t event);

#endif /* FREEWEBCAMERA_ETH_PORT_H */