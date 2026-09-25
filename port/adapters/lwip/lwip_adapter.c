/*
 * @file   lwip_adapter.c
 * @brief  lwIP bring-up for this image: the tcpip thread, the boot anchor,
 *         and the one place that names the stack together with its netif
 *         bridge.
 *
 * Implements include/net.h (the interface-layer seam). There is no wired
 * ethernet on this trunk: the only netif is the wlan bridge, which lives in
 * the net80211 adapter (port/adapters/net80211/lwip/lwip_netif.c) next to
 * the port hooks it consumes. Everything this file does is ordering:
 *
 *   1. arm the lwIP diag sink (console is up by the time a task runs),
 *   2. tcpip_init() - starts the tcpip thread; its done-callback prints the
 *      "net: READY" boot anchor from the tcpip thread, so the anchor means
 *      the stack is actually running, not merely started,
 *   3. wlan_lwip_init() - registers the rx/event hooks and adds the wlan
 *      netif (input = tcpip_input, which lwIP routes to ethernet_input for
 *      a netif carrying NETIF_FLAG_ETHARP).
 *
 * The supplicant feat takes over the event and eapol hooks when it starts;
 * the data hook and the netif stay here.
 *
 * @author zhugengyu
 * @date   25.09.2026
 */

#include "cmsis_os2.h"
#include "board.h"
#include "net.h"

#include "lwip/init.h"
#include "lwip/tcpip.h"

#include "lwip/lwip_netif.h"

/* Implemented in lwip_diag.c: tells the diagnostic sink that the console
 * exists, so lwIP assertions can be printed. */
void lwip_diag_set_console_ready(void);

/* Runs in the tcpip thread once the stack is up: the acceptance anchor. */
static void tcpip_ready(void *arg)
{
	(void)arg;

	board_log("net: READY\n");
}

int net_start(void)
{
	static int started;

	if (started) {
		return 0;
	}
	started = 1;

	lwip_diag_set_console_ready();
	board_log("net: lwip %s\n", LWIP_VERSION_STRING);

	tcpip_init(tcpip_ready, NULL);

	if (wlan_lwip_init() != 0) {
		board_log("net: wlan netif add failed\n");
		return -1;
	}

	return 0;
}
