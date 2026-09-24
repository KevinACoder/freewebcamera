/* @file   wl_load_scenario.c
 * @brief  UP debug carrier scenario 7: operator ladder + sustained load.
 *
 * The carrier has no console (app/dbg_scenario.c), so the ladder the
 * mainline drives through shell commands (wpa connect / dhcp / iperf3)
 * runs here in one pass on the scenario thread. This file sits in the
 * adapter layer because it drives lwIP/iperf3/wpa directly - app/ cannot
 * reach those headers (INC_COMMON vs INC_ADAPTER); the app side holds a
 * single extern call.
 *
 * The knobs below are plain globals: the host overrides them from the
 * stub before 'continue' (defaults are the standard wedge experiment -
 * 30s TCP uplink against the bench server).
 *
 * Load progress is lwIP's tcp.xmit/tcp.recv segment counters: the wedge
 * shape we hunt (88E VOQ jam, bulk OUT frozen) stops both. After
 * dbg_wl_stall_secs of zero movement the scenario captures the device
 * TX-queue registers (ep0 still answers in that state), logs them to the
 * ring, then parks in the stub so the host can inspect the frozen host
 * side. The device-side VOQ read must happen BEFORE the park: once the
 * (single) core sits in the stub, nothing drives ep0 anymore.
 */

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "cmsis_os2.h"
#include "gdb/gdb.h"
#include "iperf3_embedded.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include "usb.h"
#include "wlan.h"
#include "wpa_port_api.h"
#include <port/net/lwip/lwip_netif.h>

/* cross-adapter symbol, mirroring wlan_cmds.c's local extern */
extern int wlan_urtwn_reg_read(unsigned addr, unsigned *val);

/* knobs - gdb overrides before continue */
char dbg_wl_ssid[32] = "QiQiJia";
char dbg_wl_psk[64] = "paopaojie520";
volatile unsigned long dbg_wl_host = 0xc0a80012UL; /* 192.168.0.18 */
volatile unsigned long dbg_wl_secs = 30;
volatile unsigned long dbg_wl_stall_secs = 5;
volatile unsigned long dbg_wl_timeout_secs = 120; /* assoc + DHCP budget */
volatile unsigned long dbg_wl_max_parks = 2;
volatile int dbg_wl_reverse; /* 1 = downlink (-R) */

/* status - gdb reads while parked */
volatile int dbg_wl_state; /* 1 enum 2 connect 3 wait-ip 4 load 5 park 6 done 7 fail */
volatile unsigned long dbg_wl_ip;      /* acquired address, host order */
volatile unsigned long dbg_wl_xmit;    /* last lwIP tcp.xmit sample */
volatile unsigned long dbg_wl_recv;    /* last lwIP tcp.recv sample */
volatile unsigned long dbg_wl_stalled; /* consecutive stalled seconds */
volatile unsigned long dbg_wl_parks;   /* auto-parks taken */
/* captured at the park moment, ep0 still alive */
volatile unsigned long dbg_wl_voq_info;
volatile unsigned long dbg_wl_txdma_status;

/* from the driver's rtwnreg.h; captured raw instead of dumping, so the
 * values survive in a form gdb can read after the park */
#define R92C_VOQ_INFORMATION	0x400
#define R92C_TXDMA_STATUS	0x210

int wl_load_scenario_run(void);

/* usb_start is resident by design on the multi-HCD build: after bringing
 * the four buses up it stays as the hot-plug watchdog loop and never
 * returns. The mainline runs it on its own task; so does this scenario -
 * calling it inline would block the ladder forever. */
static void wl_usb_entry(void *argument)
{
	(void)argument;
	usb_start();
}

int wl_load_scenario_run(void)
{
	/* services: wlan first, usb on its own resident thread (wlan.h
	 * contract: wlan services before usb enumeration) */
	dbg_wl_state = 1;
	if (wlan_start() != 0) {
		board_log("wl: wlan FAIL\n");
		dbg_wl_state = 7;
		return -1;
	}
	if (osThreadNew(wl_usb_entry, 0, &(osThreadAttr_t){ .name = "wlusb",
			.stack_size = 2048, .priority = osPriorityBelowNormal }) == 0) {
		board_log("wl: usb thread FAIL\n");
		dbg_wl_state = 7;
		return -1;
	}

	/* connect + link + DHCP, as one retry loop: reissue the connect
	 * request every 10s (harmless while no adapter has attached yet)
	 * and wait for the lwIP bridge to hand out an address - dhcp_start
	 * fires on link-up inside the bridge. */
	dbg_wl_state = 2;
	for (unsigned long t = 0; t < dbg_wl_timeout_secs; t++) {
		struct netif *wl;
		ip4_addr_t ip;

		osDelay(1000);
		wl = wlan_lwip_get_netif();
		if (wl != NULL && netif_is_up(wl)) {
			ip = *netif_ip4_addr(wl);
			if (!ip4_addr_isany(&ip)) {
				dbg_wl_ip = ntohl(ip4_addr_get_u32(&ip));
				break;
			}
		}
		if ((t % 10UL) == 9UL) {
			if (!wpa_port_started()) {
				wlan_supp_ensure_up();
				wpa_port_start();
			}
			(void)wpa_port_connect(dbg_wl_ssid, dbg_wl_psk);
			board_log("wl: waiting link/ip %lus\n", t + 1);
		}
	}
	if (dbg_wl_ip == 0) {
		board_log("wl: no IP within %lus\n", dbg_wl_timeout_secs);
		dbg_wl_state = 7;
		return -1;
	}
	board_log("wl: IP %lu.%lu.%lu.%lu\n", (dbg_wl_ip >> 24) & 0xffUL,
		  (dbg_wl_ip >> 16) & 0xffUL, (dbg_wl_ip >> 8) & 0xffUL,
		  dbg_wl_ip & 0xffUL);

	/* sustained load against the bench server */
	dbg_wl_state = 4;
	iperf3_client_cfg_t cfg = {
		.dest_ip = htonl((uint32_t)dbg_wl_host),
		.port = 5201,
		.time_sec = (uint32_t)dbg_wl_secs,
		.interval_sec = 1,
		.bw_limit_kbps = 0,
		.udp = 0,
		.reverse = (uint8_t)dbg_wl_reverse,
	};
	if (iperf3_client_start(&cfg) != 0) {
		board_log("wl: iperf start FAIL\n");
		dbg_wl_state = 7;
		return -1;
	}

	unsigned long last_xmit = lwip_stats.tcp.xmit;
	unsigned long last_recv = lwip_stats.tcp.recv;
	unsigned long stall = 0;
	for (unsigned long t = 0; t < dbg_wl_secs + 60; t++) {
		unsigned v;

		osDelay(1000);
		dbg_wl_xmit = lwip_stats.tcp.xmit;
		dbg_wl_recv = lwip_stats.tcp.recv;
		board_log("wl: t=%lus xmit=%lu recv=%lu\n", t, dbg_wl_xmit,
			  dbg_wl_recv);
		if (dbg_wl_xmit != last_xmit || dbg_wl_recv != last_recv) {
			stall = 0;
			dbg_wl_stalled = 0;
		} else {
			stall++;
			dbg_wl_stalled = stall;
			if (stall >= dbg_wl_stall_secs &&
			    dbg_wl_parks < dbg_wl_max_parks) {
				/* wedge shape: grab device truth while the
				 * core (and ep0) are still running */
				dbg_wl_state = 5;
				if (wlan_urtwn_reg_read(R92C_VOQ_INFORMATION,
							&v) == 0) {
					dbg_wl_voq_info = v;
				}
				if (wlan_urtwn_reg_read(R92C_TXDMA_STATUS,
							&v) == 0) {
					dbg_wl_txdma_status = v;
				}
				board_log("wl: STALL %lus - VOQ=0x%lx "
					  "TXDMA=0x%lx, park for gdb\n",
					  stall, dbg_wl_voq_info,
					  dbg_wl_txdma_status);
				dbg_wl_parks++;
				gdb_break();
				/* host inspected and continued */
				stall = 0;
				dbg_wl_stalled = 0;
				dbg_wl_state = 4;
				last_xmit = lwip_stats.tcp.xmit;
				last_recv = lwip_stats.tcp.recv;
			}
		}
		last_xmit = dbg_wl_xmit;
		last_recv = dbg_wl_recv;
		if (!iperf3_client_is_running()) {
			board_log("wl: iperf done\n");
			break;
		}
	}
	(void)iperf3_client_stop();
	dbg_wl_state = 6;
	board_log("wl: done\n");
	return 0;
}
