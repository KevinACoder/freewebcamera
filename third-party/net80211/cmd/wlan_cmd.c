/*
 * @file
 * @brief wlan command: bring the device up, trigger a scan and print
 * the candidates net80211 collected.
 *
 * Everything dispatches through the bus-neutral port core, so the
 * command is driver-agnostic.
 *
 * @date 08.09.2026
 * @author zhugengyu
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel/time/ktime.h>

#include <port/port.h>

extern void wlan_usbdi_trace_reset(void);
extern void wlan_usbdi_trace_set(unsigned level);
extern void wlan_usbdi_stats_dump(void);
extern int aes_ccm_selftest(void);

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

/* 88E firmware-maintenance hook (urtwn adapter, see "wlan ra") */
extern unsigned wlan_ra_hook_get_enabled(void);
extern void wlan_ra_hook_set_enabled(unsigned on);
extern void wlan_ra_hook_force(void);
extern void wlan_ra_hook_kick(void);

int main(int argc, char **argv) {
	int wait_s = 10;
	const char *dev = NULL;

	/* an optional trailing driver name focuses the hook */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "urtwn") == 0 || strcmp(argv[i], "iwm") == 0) {
			dev = argv[i];
		}
	}
	if (dev != NULL && wlan_port_select(dev) != 0) {
		printf("wlan: no such device %s\n", dev);
		return 1;
	}

	if (argc > 1 && strcmp(argv[1], "status") == 0) {
		wlan_port_status_dump();
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "ccmtest") == 0) {
		int ret = aes_ccm_selftest();
		printf("AES-CCM RFC3610: %s\n", ret ? "FAIL" : "PASS");
		return ret != 0;
	}

	if (argc > 1 && strcmp(argv[1], "scan") == 0) {
		if (argc > 2) {
			wait_s = atoi(argv[2]);
		}
		if (wait_s < 1) {
			wait_s = 1;
		}
		wlan_usbdi_trace_reset();
		wlan_port_up();
		wlan_port_scan(NULL, 0);
		printf("scanning for %d s...\n", wait_s);
		ksleep((unsigned) wait_s * 1000);
		printf("scan results:\n");
		wlan_port_scan_dump();
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "trace") == 0) {
		/* 0 = quiet (default), 1 = async events, 2 = + control xfers */
		wlan_usbdi_trace_set((argc > 2) ? (unsigned) atoi(argv[2]) : 0);
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "usbstats") == 0) {
		wlan_usbdi_stats_dump();
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "reg") == 0) {
		unsigned addr, val;

		if (argc >= 3 && strcmp(argv[2], "txq") == 0) {
			wlan_urtwn_txq_dump();
			return 0;
		}
		if (argc >= 4 && strcmp(argv[2], "read") == 0) {
			addr = (unsigned) strtoul(argv[3], NULL, 16);
			if (wlan_urtwn_reg_read(addr, &val) == 0) {
				printf("urtwn reg[0x%04x] = 0x%08x\n",
				    addr & 0xffffu, val);
			} else {
				printf("wlan: reg read failed\n");
			}
			return 0;
		}
		if (argc >= 5 && strcmp(argv[2], "write") == 0) {
			addr = (unsigned) strtoul(argv[3], NULL, 16);
			val = (unsigned) strtoul(argv[4], NULL, 16);
			printf("wlan: reg write %s\n",
			    wlan_urtwn_reg_write(addr, val) == 0 ?
			    "ok" : "failed");
			return 0;
		}
		printf("usage: wlan reg read <hexaddr> | "
		    "wlan reg write <hexaddr> <hexval> | wlan reg txq\n");
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "calib") == 0) {
		if (argc > 2) {
			wlan_callout_set_enabled(
			    (unsigned) atoi(argv[2]) != 0);
		}
		printf("wlan callouts enabled=%u fires=%u sched=%u "
		    "suppressed=%u\n",
		    wlan_callout_get_enabled(), wlan_callout_fires,
		    wlan_callout_sched, wlan_callout_suppressed);
		return 0;
	}

	if (argc > 1 && strcmp(argv[1], "ra") == 0) {
		if (argc > 2) {
			wlan_ra_hook_set_enabled(
			    (unsigned) atoi(argv[2]) != 0);
			if (wlan_ra_hook_get_enabled()) {
				wlan_ra_hook_kick();
			}
		}
		printf("wlan ra hook enabled=%u (fw maintenance: "
		    "joinbss_rpt + pwrmode + ra mask per assoc)\n",
		    wlan_ra_hook_get_enabled());
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "fwfix") == 0) {
		wlan_ra_hook_force();
		return 0;
	}

	printf("usage: wlan scan [seconds] | wlan status | wlan ccmtest | "
	    "wlan trace [0|1|2] | wlan usbstats | "
	    "wlan reg read|write|txq | wlan calib [0|1] | "
	    "wlan ra [0|1] | wlan fwfix\n");
	return 0;
}
