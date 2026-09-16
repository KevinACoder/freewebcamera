/*
 * @file   ethernetif.c
 * @brief  The lwIP netif glue for one Ethernet port: nothing here touches
 *         registers or lwIP internals.
 *
 * Direction of travel, receive: the MAC driver stages received frames and
 * raises ARM_ETH_MAC_EVENT_RX_FRAME from its interrupt. That event sets a
 * thread flag; the per-port receive thread wakes, reads frames out through
 * ReadFrame and hands each one to netif->input (tcpip_input, so the frame
 * reaches the stack's thread). The thread waits with a timeout as well as on
 * the flag: the driver's watchdog stages frames without raising an event, and a
 * receive path that only ever wakes on an interrupt has no way to notice a
 * lost one.
 *
 * Direction of travel, transmit: lwIP calls linkoutput from its own thread.
 * The frame is linearized into a per-port buffer (the MAC takes one contiguous
 * frame; lwIP works in pbufs) and handed to SendFrame, which owns the
 * descriptors and the cache maintenance.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#include <string.h>

#include "cmsis_os2.h"
#include "cmsis_os2_ext.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"

#include "eth_port.h"

/* --- transmit ------------------------------------------------------------- */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	struct eth_port *port = netif->state;
	uint16_t len = p->tot_len;

	if (len > ETH_PORT_MAX_FRAME) {
		port->err_drops++;
		return ERR_MEM;
	}

	/* One contiguous frame for the MAC: copy the chain out rather than
	 * asking the driver to walk pbufs (it deliberately knows nothing about
	 * lwIP). */
	(void)pbuf_copy_partial(p, port->tx_buf, len, 0);

	if (port->mac->SendFrame(port->tx_buf, len, 0) != ARM_DRIVER_OK) {
		/* The driver waits briefly for a descriptor before saying busy;
		 * reporting ERR_MEM here is lwIP's cue to retry. */
		port->err_drops++;
		return ERR_MEM;
	}

	port->tx_frames++;

	return ERR_OK;
}

/* --- receive -------------------------------------------------------------- */

/* Drain everything the driver has staged. Called from the receive thread. */
void ethernetif_input(struct eth_port *port)
{
	for (;;) {
		uint32_t size = port->mac->GetRxFrameSize();
		struct pbuf *p;

		if (size == 0) {
			break;
		}
		p = pbuf_alloc(PBUF_RAW, (u16_t)size, PBUF_POOL);
		if (p == NULL) {
			/* Pool exhausted: leave the frame staged and try again on
			 * the next wakeup rather than dropping it here. */
			break;
		}
		if ((size > p->len) ||
		    (port->mac->ReadFrame((uint8_t *)p->payload, size) != (int32_t)size)) {
			pbuf_free(p);
			port->err_drops++;
			break;
		}

		if (port->netif.input(p, &port->netif) != ERR_OK) {
			pbuf_free(p);
		}
		port->rx_frames++;
	}
}

void ethernetif_rx_thread(void *argument)
{
	struct eth_port *port = argument;

	for (;;) {
		/* Event-driven with a timeout: the flag gives the low-latency
		 * path, the timeout the self-healing one (see the file header). */
		(void)osThreadFlagsWait(ETH_PORT_RX_FLAG, osFlagsWaitAny, 20u);
		ethernetif_input(port);
	}
}

/* The MAC driver's event callback. Runs in interrupt context, hence the
 * thread flags and the FromISR setter; the flag latches, so a frame that
 * arrives while the receive thread is still working is not lost. */
void ethernetif_event(struct eth_port *port, uint32_t event)
{
	if ((event & ARM_ETH_MAC_EVENT_RX_FRAME) != 0U) {
		port->rx_events++;
		if (port->rx_thread != NULL) {
			(void)osThreadFlagsSetFromISR(port->rx_thread, ETH_PORT_RX_FLAG);
		}
	}
}

/* --- netif setup ---------------------------------------------------------- */

err_t ethernetif_init(struct netif *netif)
{
	struct eth_port *port = netif->state;
	ARM_ETH_MAC_ADDR addr;

	netif->name[0] = 'e';
	netif->name[1] = (char)('0' + port->index);
	netif->output = etharp_output;
	netif->linkoutput = low_level_output;
	netif->mtu = 1500;
	netif->hwaddr_len = ETH_HWADDR_LEN;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;

	/* The MAC address comes from the driver (each port has its own, from
	 * the board tables); that is also how the host's ARP table stays
	 * readable across ports. */
	if (port->mac->GetMacAddress(&addr) == ARM_DRIVER_OK) {
		memcpy(netif->hwaddr, addr.b, ETH_HWADDR_LEN);
	}

	return ERR_OK;
}