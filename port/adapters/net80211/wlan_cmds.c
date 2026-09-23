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

/* register-level debug access (urtwn adapter) */
extern int wlan_urtwn_reg_read(unsigned addr, unsigned *val);
extern int wlan_urtwn_reg_write(unsigned addr, unsigned val);
extern void wlan_urtwn_txq_dump(void);

/* register-level debug access (rtw8189f adapter; "wlan reg" falls back
 * to it when the USB adapter has no device) */
extern int wlan_rtw8189f_reg_read(unsigned addr, unsigned *val);
extern int wlan_rtw8189f_reg_write(unsigned addr, unsigned val);
extern void wlan_rtw8189f_txq_dump(void);
extern void wlan_rtw8189f_icstats_dump(void);
extern void wlan_rtw8189f_sdreg_dump(void);

/* lwIP bridge drop counters (net_80211 port/net/lwip/lwip_netif.c) */
extern void wlan_lwip_bridge_dump(void);

/* callout diagnostic gate (osal layer, see "wlan calib") */
extern volatile unsigned wlan_callout_fires;
extern volatile unsigned wlan_callout_sched;
extern volatile unsigned wlan_callout_suppressed;
extern unsigned wlan_callout_get_enabled(void);
extern void wlan_callout_set_enabled(unsigned on);

/* 88E firmware-maintenance hook (urtwn adapter, see "wlan ra") */
extern unsigned wlan_ra_hook_get_enabled(void);
extern void wlan_ra_hook_set_enabled(unsigned on);
extern void wlan_ra_hook_force(void);
extern void wlan_ra_hook_kick(void);

/* the cherrysh libc strtoul ignores the base argument (parses decimal
 * regardless), so hex addresses need this tiny parser */
static unsigned long parse_hex(const char *s)
{
	unsigned long v = 0;

	while (*s == 'x' || *s == 'X') {
		s++;
	}
	while (*s != '\0') {
		char c = *s;

		if (c >= '0' && c <= '9') {
			v = v * 16UL + (unsigned long) (c - '0');
		} else if (c >= 'a' && c <= 'f') {
			v = v * 16UL + (unsigned long) (c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			v = v * 16UL + (unsigned long) (c - 'A' + 10);
		} else {
			break;
		}
		s++;
	}
	return v;
}

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

	if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
		/* RX-path health in one shot: net80211 ic_stats (dup/replay/
		 * demic/decap/...), the lwIP bridge drop counters, and the
		 * rtw8189f driver frame counters. Snapshot twice ~10 s apart
		 * across a wedged flow and diff. */
		wlan_rtw8189f_icstats_dump();
		wlan_lwip_bridge_dump();
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "sdreg") == 0) {
		/* SDIO-local window: HISR/HIMR/RX0_REQ_LEN/FREE_TXPG - the
		 * interrupt and RX-fifo state the MAC window cannot show */
		wlan_rtw8189f_sdreg_dump();
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "reg") == 0) {
		unsigned long addr;
		unsigned val;

		/* dispatch order = adapter priority: rtw8189f (SDIO, the
		 * netif owner when present) first, urtwn (USB dongle)
		 * second - with both attached the urtwn read would still
		 * succeed and dump the wrong chip. */
		if (argc >= 3 && strcmp(argv[2], "txq") == 0) {
			if (wlan_rtw8189f_reg_read(0x0100u, &val) == 0) {
				wlan_rtw8189f_txq_dump();
			} else {
				wlan_urtwn_txq_dump();
			}
			return 0;
		}
		if (argc >= 4 && strcmp(argv[2], "read") == 0) {
			addr = parse_hex(argv[3]);
			if (wlan_rtw8189f_reg_read((unsigned) addr, &val) == 0) {
				csh_printf(csh, "rtw8189f reg[0x%04lx] = 0x%08x\r\n",
					   addr & 0xfffffful, val);
			} else if (wlan_urtwn_reg_read((unsigned) addr,
			    &val) == 0) {
				csh_printf(csh, "urtwn reg[0x%04lx] = 0x%08x\r\n",
					   addr & 0xfffful, val);
			} else {
				csh_printf(csh, "wlan: reg read failed\r\n");
			}
			return 0;
		}
		if (argc >= 5 && strcmp(argv[2], "write") == 0) {
			addr = parse_hex(argv[3]);
			val = (unsigned) parse_hex(argv[4]);
			if (wlan_rtw8189f_reg_write((unsigned) addr, val) == 0) {
				csh_printf(csh, "wlan: reg write ok (rtw8189f)\r\n");
			} else if (wlan_urtwn_reg_write((unsigned) addr,
			    val) == 0) {
				csh_printf(csh, "wlan: reg write ok (urtwn)\r\n");
			} else {
				csh_printf(csh, "wlan: reg write failed\r\n");
			}
			return 0;
		}
		csh_printf(csh, "usage: wlan reg read <hexaddr> | "
			   "wlan reg write <hexaddr> <hexval> | wlan reg txq\r\n");
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "calib") == 0) {
		if (argc > 2) {
			wlan_callout_set_enabled(
			    (unsigned) atoi(argv[2]) != 0);
		}
		csh_printf(csh, "wlan callouts enabled=%u fires=%u sched=%u "
			   "suppressed=%u\r\n",
			   wlan_callout_get_enabled(), wlan_callout_fires,
			   wlan_callout_sched, wlan_callout_suppressed);
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

	if (argc >= 2 && strcmp(argv[1], "ra") == 0) {
		if (argc > 2) {
			wlan_ra_hook_set_enabled(
			    (unsigned) atoi(argv[2]) != 0);
			if (wlan_ra_hook_get_enabled()) {
				wlan_ra_hook_kick();
			}
		}
		csh_printf(csh, "wlan ra hook enabled=%u (fw maintenance: "
			   "joinbss_rpt + pwrmode + ra mask per assoc)\r\n",
			   wlan_ra_hook_get_enabled());
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "fwfix") == 0) {
		wlan_ra_hook_force();
		return 0;
	}

	csh_printf(csh,
		   "usage: wlan scan [seconds] | wlan status | wlan net | "
		   "wlan trace [0|1|2] | wlan usbstats | wlan stats | "
		   "wlan sdreg | "
		   "wlan reg read|write|txq | wlan calib [0|1] | "
		   "wlan ra [0|1] | wlan fwfix\r\n");
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_wlan, wlan, "wlan scan [s] | status | trace [n] | usbstats | stats | sdreg | reg | calib | ra | fwfix",
			  "net80211 adapter: bring up the radio and scan");
