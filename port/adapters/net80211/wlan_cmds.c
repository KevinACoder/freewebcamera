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
#include <port/net/lwip/lwip_netif.h>

#include "wlan_adapter.h"
#include "wpa_port_api.h"

extern int rtw8189f_data_rate_set(unsigned mbps);
extern unsigned rtw8189f_data_rate_get(void);

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
		const char *name = wlan_port_active_name();
		uint8_t mac[6];

		if (name != NULL && wlan_port_get_hwaddr(mac) == 0) {
			csh_printf(csh, "wlan selected=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
			    name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
		wlan_port_status_dump();
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "select") == 0) {
		if (argc != 3 || (strcmp(argv[2], "urtwn") != 0 &&
		    strcmp(argv[2], "rtw8189f") != 0 &&
		    strcmp(argv[2], "rtw88u") != 0)) {
			csh_printf(csh, "usage: wlan select urtwn|rtw8189f|rtw88u\r\n");
		} else if (wpa_port_started()) {
			csh_printf(csh, "wlan: select before wpa start\r\n");
		} else if (wlan_port_select(argv[2]) != 0) {
			csh_printf(csh, "wlan: adapter %s not attached\r\n", argv[2]);
		} else {
			csh_printf(csh, "wlan: selected %s\r\n", argv[2]);
		}
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "rate") == 0) {
		if (strcmp(wlan_port_active_name() != NULL ?
		    wlan_port_active_name() : "", "rtw8189f") != 0) {
			csh_printf(csh, "wlan: select rtw8189f first\r\n");
			return 0;
		}
		if (argc > 3 || (argc == 3 &&
		    strcmp(argv[2], "24") != 0 &&
		    strcmp(argv[2], "36") != 0 &&
		    strcmp(argv[2], "54") != 0)) {
			csh_printf(csh, "usage: wlan rate 24|36|54\r\n");
			return 0;
		}
		if (argc == 3)
			(void) rtw8189f_data_rate_set((unsigned) atoi(argv[2]));
		csh_printf(csh, "rtw8189f data rate=%u Mbps\r\n",
		    rtw8189f_data_rate_get());
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
		int sdio = wlan_port_active_name() != NULL &&
		    strcmp(wlan_port_active_name(), "rtw8189f") == 0;

		/* Diagnostics follow the selected adapter, including when both
		 * buses are attached. */
		if (argc >= 3 && strcmp(argv[2], "txq") == 0) {
			if (sdio) {
				wlan_rtw8189f_txq_dump();
			} else {
				wlan_urtwn_txq_dump();
			}
			return 0;
		}
		if (argc >= 4 && strcmp(argv[2], "read") == 0) {
			addr = parse_hex(argv[3]);
			if (sdio && wlan_rtw8189f_reg_read((unsigned) addr, &val) == 0) {
				csh_printf(csh, "rtw8189f reg[0x%04lx] = 0x%08x\r\n",
					   addr & 0xfffffful, val);
			} else if (!sdio && wlan_urtwn_reg_read((unsigned) addr,
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
			if (sdio && wlan_rtw8189f_reg_write((unsigned) addr, val) == 0) {
				csh_printf(csh, "wlan: reg write ok (rtw8189f)\r\n");
			} else if (!sdio && wlan_urtwn_reg_write((unsigned) addr,
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
		struct netif *wl;
		char ipbuf[16], gwbuf[16], maskbuf[16];

		(void) wlan_lwip_start();
		wl = wlan_lwip_get_netif();
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

	if (argc >= 2 && strcmp(argv[1], "dbg") == 0 && argc == 3) {
		/* runtime rtw88 debug mask (RTW_DBG_USB|RX|... bits from
		 * dist/main.h); compiled in only with CONFIG_RTW88_DEBUG.
		 * parse_hex, not strtoul: the shell libc's strtoul ignores
		 * the base argument, "0x00080004" parsed as 0 */
		extern unsigned int rtw_debug_mask;
		rtw_debug_mask = (unsigned int) parse_hex(argv[2]);
		csh_printf(csh, "wlan: rtw_debug_mask=0x%x\r\n",
			   rtw_debug_mask);
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "rawdump") == 0) {
		/* retained-memory raw RX capture (rtw88_usb.c): demux
		 * ground truth without racing the console flood */
		extern void rtw88_usb_rawdump(void);
		rtw88_usb_rawdump();
		return 0;
	}

	csh_printf(csh,
		   "usage: wlan select urtwn|rtw8189f|rtw88u | rate 24|36|54 | "
		   "scan [seconds] | status | net | "
		   "wlan trace [0|1|2] | wlan usbstats | wlan stats | "
		   "wlan sdreg | "
		   "wlan reg read|write|txq | wlan calib [0|1] | "
		   "wlan ra [0|1] | wlan fwfix | wlan dbg <mask> | "
		   "wlan rawdump\r\n");
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_wlan, wlan,
        "wlan select urtwn|rtw8189f|rtw88u | rate 24|36|54 | "
        "scan [s] | status | net | trace [n] | usbstats | stats | "
        "sdreg | reg | calib | ra | fwfix | dbg <mask> | rawdump",
			  "net80211 adapter: bring up the radio and scan");
