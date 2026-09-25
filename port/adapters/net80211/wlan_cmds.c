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

/* register-level debug access (urtwn adapter) */
extern int wlan_urtwn_reg_read(unsigned addr, unsigned *val);
extern int wlan_urtwn_reg_write(unsigned addr, unsigned val);
extern void wlan_urtwn_txq_dump(void);

/* callout diagnostic gate (osal layer, see "wlan calib") */
extern volatile unsigned wlan_callout_fires;
extern volatile unsigned wlan_callout_sched;
extern volatile unsigned wlan_callout_suppressed;
extern unsigned wlan_callout_get_enabled(void);
extern void wlan_callout_set_enabled(unsigned on);

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

	csh_printf(csh,
		   "usage: wlan start | scan [seconds] | status | "
		   "wlan reg read|write|txq | wlan calib [0|1]\r\n");
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_wlan, wlan, "wlan start | scan [s] | status | reg",
			  "net80211 over NetBSD usb: bring up the radio and scan");
