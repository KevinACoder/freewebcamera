/*
 * @file
 * @brief wlan shell command: bring the USB platform up, trigger a scan
 * and print the candidates net80211 collected.
 *
 * Everything dispatches through the bus-neutral port core, so the
 * command shape mirrors the library's embox cmd/wlan_cmd.c; the
 * console is cherrysh here.
 *
 * @author zhugengyu
 * @date 25.09.2026
 */

#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include "cherrysh_adapter.h"
#include "csh.h"

#include "port.h"

#include "wlan_adapter.h"

#if WLAN_NIC_USB
extern void usb_platform_dump(void);
extern void usb_platform_qh_dump(void);
extern void usb_platform_reg_dump(void);
extern void usb_platform_hist_dump(unsigned int max);
extern void usb_platform_delay_test(void);
extern void usb_xhci_dump(void);
extern void usb_xhci_reg_dump(void);

/* register-level debug access (urtwn adapter) */
extern int wlan_urtwn_reg_read(unsigned addr, unsigned *val);
extern int wlan_urtwn_reg_write(unsigned addr, unsigned val);
extern void wlan_urtwn_txq_dump(void);
extern void wlan_urtwn_chanmap_dump(void);
#endif /* WLAN_NIC_USB */

#if WLAN_NIC_SDIO
/* register-level debug access (rtw8189f adapter; "wlan reg" falls back
 * to it when no urtwn adapter is compiled in) */
extern int rtw8189f_data_rate_set(unsigned mbps);
extern unsigned rtw8189f_data_rate_get(void);
extern int wlan_rtw8189f_reg_read(unsigned addr, unsigned *val);
extern int wlan_rtw8189f_reg_write(unsigned addr, unsigned val);
extern void wlan_rtw8189f_txq_dump(void);
extern void wlan_rtw8189f_icstats_dump(void);
extern void wlan_rtw8189f_sdreg_dump(void);
#endif

/* callout diagnostic gate (osal layer, see "wlan calib") */
extern volatile unsigned wlan_callout_fires;
extern volatile unsigned wlan_callout_sched;
extern volatile unsigned wlan_callout_suppressed;
extern unsigned wlan_callout_get_enabled(void);
extern void wlan_callout_set_enabled(unsigned on);

/* first-scan forensics (osal layer): per-callout registry walk */
extern void wlan_callout_registry_dump(void);

/* cv wakeup forensics (osal layer): 0-waiter signals release no token */
extern volatile unsigned wlan_cv_signals;
extern volatile unsigned wlan_cv_signals_dropped;
extern volatile unsigned wlan_cv_broadcasts;
extern volatile unsigned wlan_cv_broadcasts_dropped;

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

	if (argc >= 2 && strcmp(argv[1], "start") == 0) {
		extern int wlan_start(void);
		extern int wlan_adapter_ready(void);

		if (wlan_adapter_ready()) {
			csh_printf(csh, "wlan: already started\r\n");
			return 0;
		}
		csh_printf(csh, "wlan: usb platform + enumeration...\r\n");
		if (wlan_start() != 0) {
			csh_printf(csh, "wlan: start failed\r\n");
		} else {
			csh_printf(csh, "wlan: started\r\n");
		}
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "status") == 0) {
		const char *name = wlan_port_active_name();
		uint8_t mac[6];

		if (name != NULL && wlan_port_get_hwaddr(mac) == 0) {
			csh_printf(csh, "wlan selected=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
			    name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		} else {
			csh_printf(csh, "wlan: no adapter registered yet "
				   "(run: wlan start)\r\n");
		}
		wlan_port_status_dump();
#if WLAN_NIC_USB
		usb_platform_dump();
#endif
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
		if (wlan_port_up() != 0) {
			csh_printf(csh, "wlan: radio not up\r\n");
			return 0;
		}
		wlan_port_scan(NULL, 0);
		csh_printf(csh, "wlan: scanning for %d s...\r\n", wait_s);
		osDelay((uint32_t) wait_s * osKernelGetTickFreq());
		csh_printf(csh, "wlan: scan results:\r\n");
		wlan_port_scan_dump();
		return 0;
	}

#if WLAN_NIC_USB
	if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
		usb_platform_dump();
		usb_platform_qh_dump();
		usb_platform_reg_dump();
		return 0;
	}

	/* the xHCI state and its command/event-ring windows: the place a
	 * "command timeout" actually gets decided - the completions may
	 * be sitting in the ring unconsumed (feat/xhci evidence) */
	if (argc >= 2 && strcmp(argv[1], "xhci") == 0) {
		usb_xhci_dump();
		return 0;
	}

	/* raw xhci register rows over the three windows */
	if (argc >= 2 && strcmp(argv[1], "xreg") == 0) {
		usb_xhci_reg_dump();
		return 0;
	}

	/* the usb history ring: what the imported core logged, which is
	 * the only place a failed enumeration says "why" */
	if (argc >= 2 && (strcmp(argv[1], "hist") == 0 ||
			  strcmp(argv[1], "history") == 0)) {
		unsigned max = 0;

		if (argc > 2) {
			max = (unsigned) atoi(argv[2]);
		}
		usb_platform_hist_dump(max);
		return 0;
	}

	/* requested vs measured waits: the EHCI port reset holds PR for
	 * 250 ms through this path, and a short wait would look exactly
	 * like a dead PHY */
	if (argc >= 2 && strcmp(argv[1], "delaytest") == 0) {
		usb_platform_delay_test();
		return 0;
	}

	/* the urtwn driver's register windows */
	if (argc >= 2 && strcmp(argv[1], "reg") == 0) {
		unsigned long addr;
		unsigned val;

		if (argc >= 3 && strcmp(argv[2], "txq") == 0) {
			wlan_urtwn_txq_dump();
			return 0;
		}
		if (argc >= 4 && strcmp(argv[2], "read") == 0) {
			addr = parse_hex(argv[3]);
			if (wlan_urtwn_reg_read((unsigned) addr, &val) == 0) {
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
			if (wlan_urtwn_reg_write((unsigned) addr, val) == 0) {
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
#endif /* WLAN_NIC_USB */

#if WLAN_NIC_SDIO
	/* the rtw8189f driver's register windows + SDIO-local state */
	if (argc >= 2 && strcmp(argv[1], "reg") == 0) {
		unsigned long addr;
		unsigned val;

		if (argc >= 3 && strcmp(argv[2], "txq") == 0) {
			wlan_rtw8189f_txq_dump();
			return 0;
		}
		if (argc >= 3 && strcmp(argv[2], "sdreg") == 0) {
			wlan_rtw8189f_sdreg_dump();
			return 0;
		}
		if (argc >= 3 && strcmp(argv[2], "icstats") == 0) {
			/* net80211 RX-path error counters: the TCP-downlink
			 * chase reads them (plus the driver frame counters
			 * in wlan dump) twice ~10 s apart and diffs */
			wlan_rtw8189f_icstats_dump();
			return 0;
		}
		if (argc >= 4 && strcmp(argv[2], "read") == 0) {
			addr = parse_hex(argv[3]);
			if (wlan_rtw8189f_reg_read((unsigned) addr, &val) == 0) {
				csh_printf(csh, "rtw8189f reg[0x%04lx] = 0x%08x\r\n",
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
			} else {
				csh_printf(csh, "wlan: reg write failed\r\n");
			}
			return 0;
		}
		csh_printf(csh, "usage: wlan reg read <hexaddr> | "
			   "wlan reg write <hexaddr> <hexval> | "
			   "wlan reg txq | wlan reg sdreg | wlan reg icstats\r\n");
		return 0;
	}

	/* the r4 TX-rate override for data frames (the throughput candidate
	 * knob; 24 Mbps is the tuned default) */
	if (argc >= 2 && strcmp(argv[1], "rate") == 0) {
		if (argc >= 3) {
			(void) rtw8189f_data_rate_set((unsigned) atoi(argv[2]));
		}
		csh_printf(csh, "rtw8189f data rate=%u Mbps\r\n",
		    rtw8189f_data_rate_get());
		return 0;
	}
#endif /* WLAN_NIC_SDIO */

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

	/* per-callout counters: a stalled first scan says which timer
	 * stopped being re-armed (scheds freeze) vs which callback
	 * stopped running (fires freeze) vs timer-thread blocking
	 * (long fires) */
	if (argc >= 2 && strcmp(argv[1], "callouts") == 0) {
		wlan_callout_registry_dump();
		return 0;
	}

	/* scan state machine + channel bitmap + host cmd ring: the
	 * parked-first-scan state in one screen */
#if WLAN_NIC_USB
	if (argc >= 2 && strcmp(argv[1], "chanmap") == 0) {
		wlan_urtwn_chanmap_dump();
		return 0;
	}
#endif

	if (argc >= 2 && strcmp(argv[1], "cv") == 0) {
		csh_printf(csh, "wlan cv: signals=%u dropped=%u "
			   "broadcasts=%u bdropped=%u\r\n",
			   wlan_cv_signals, wlan_cv_signals_dropped,
			   wlan_cv_broadcasts, wlan_cv_broadcasts_dropped);
		return 0;
	}

	/* runtime usb history level (wlan_start pins it to 10; the full
	 * ring flood drowns the interesting records) */
#if WLAN_NIC_USB
	if (argc >= 2 && strcmp(argv[1], "usbdebug") == 0) {
		extern int usbdebug;

		if (argc > 2) {
			usbdebug = atoi(argv[2]);
		}
		csh_printf(csh, "wlan: usbdebug=%d\r\n", usbdebug);
		return 0;
	}
#endif

	csh_printf(csh,
		   "usage: wlan start | scan [seconds] | status | dump | "
		   "hist [n] | delaytest | reg read|write|txq | calib [0|1] | "
		   "callouts | chanmap | cv | usbdebug <n>\r\n");
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_wlan, wlan, "wlan start | scan [s] | status | hist",
			  "net80211 over NetBSD usb: bring up the radio and scan");
