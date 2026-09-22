/*
 * @file
 * @brief wlan command: bring the device up, trigger a scan and print
 * the candidates net80211 collected.
 *
 * Everything dispatches through the bus-neutral port core, so the
 * command is driver-agnostic. The command shape mirrors the library's
 * embox cmd/wlan_cmd.c; the console is cherrysh here.
 *
 * @author zhugengyu
 * @date 22.09.2026
 */

#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include "cherrysh_adapter.h"
#include "csh.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"

#include <port/port.h>

#include "wlan_adapter.h"

extern void wlan_usbdi_trace_reset(void);
extern void wlan_usbdi_trace_set(unsigned level);
extern void wlan_usbdi_stats_dump(void);

static int cmd_wlan(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	if (argc >= 2 && strcmp(argv[1], "status") == 0) {
		wlan_port_status_dump();
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
		int wait_s = 10;

		if (argc > 2) {
			wait_s = atoi(argv[2]);
		}
		if (wait_s < 1) {
			wait_s = 1;
		}
		(void) wlan_lwip_start();
		wlan_usbdi_trace_reset();
		wlan_port_up();
		wlan_port_scan(NULL, 0);
		csh_printf(csh, "wlan: scanning for %d s...\r\n", wait_s);
		osDelay((uint32_t) wait_s * osKernelGetTickFreq());
		csh_printf(csh, "wlan: scan results:\r\n");
		wlan_port_scan_dump();
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "trace") == 0) {
		/* 0 = quiet (default), 1 = async events, 2 = + control xfers */
		wlan_usbdi_trace_set((argc > 2) ? (unsigned) atoi(argv[2]) : 0);
		csh_printf(csh, "wlan: usbdi trace level set\r\n");
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "usbstats") == 0) {
		wlan_usbdi_stats_dump();
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "net") == 0) {
		/* wl netif view: address/gw/lease - the lwip-side state the
		 * radio-side "status" cannot show */
		struct netif *wl = wlan_lwip_get_netif();
		char ipbuf[16], gwbuf[16], maskbuf[16];

		(void) wlan_lwip_start();
		if (wl == NULL || !netif_is_up(wl)) {
			csh_printf(csh, "wlan net: netif not up\r\n");
			return 0;
		}
		/* ip4addr_ntoa shares one static scratch - always the _r form
		 * when printing more than one address per line */
		ip4addr_ntoa_r(netif_ip4_addr(wl), ipbuf, sizeof(ipbuf));
		ip4addr_ntoa_r(netif_ip4_gw(wl), gwbuf, sizeof(gwbuf));
		ip4addr_ntoa_r(netif_ip4_netmask(wl), maskbuf, sizeof(maskbuf));
		csh_printf(csh, "wl: ip=%s gw=%s mask=%s\r\n",
			   ipbuf, gwbuf, maskbuf);
		csh_printf(csh, "wl: link=%s dhcp_bound=%s\r\n",
			   netif_is_link_up(wl) ? "up" : "down",
			   dhcp_supplied_address(wl) ? "yes" : "no");
		return 0;
	}

	csh_printf(csh,
		   "usage: wlan scan [seconds] | wlan status | wlan net | "
		   "wlan trace [0|1|2] | wlan usbstats\r\n");
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_wlan, wlan, "wlan scan [s] | status | trace [n] | usbstats",
			  "net80211 adapter: bring up the radio and scan");
