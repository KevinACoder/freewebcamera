/*
 * @file
 * @brief wpa command: drive the supplicant from the cherrysh console.
 *
 * The PSK is only ever a command line argument, nothing is stored.
 *
 * @author zhugengyu
 * @date 22.09.2026
 */

#include <string.h>

#include "cherrysh_adapter.h"
#include "csh.h"

#include "wpa_port_api.h"

#include "wlan_adapter.h"

/* sscanf sits behind newlib's scanf machinery the image does not ship;
 * a MAC address is six hex bytes, parse it by hand */
static int sscanf_bssid(const char *s, unsigned int b[6]);

static void usage(chry_shell_t *csh) {
	csh_printf(csh,
		   "usage: wpa start | status | connect <ssid> <psk> [bssid] | "
		   "disconnect\r\n");
}

static int cmd_wpa(int argc, char **argv) {
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	int ret;

	if (argc < 2) {
		usage(csh);
		return 0;
	}

	if (strcmp(argv[1], "start") == 0) {
		(void) wlan_lwip_start();
		ret = wpa_port_start();
		csh_printf(csh, "wpa: start %s\r\n",
			   ret == 0 ? "ok" : "failed");
		return 0;
	}
	if (strcmp(argv[1], "status") == 0) {
		ret = wpa_port_status();
		if (ret != 0) {
			csh_printf(csh, "wpa: supplicant busy\r\n");
		}
		return 0;
	}
	if (strcmp(argv[1], "connect") == 0 && (argc == 4 || argc == 5)) {
		if (!wpa_port_started()) {
			/* interface up happens on this (console) thread:
			 * if_init blocks on the USB workers and must not
			 * run on the supplicant thread */
			(void) wlan_lwip_start();
			wlan_supp_ensure_up();
			wpa_port_start();
		}
		if (argc == 5) {
			unsigned int b[6];
			unsigned char mac[6];
			int i;

			if (sscanf_bssid(argv[4], b) == 6) {
				for (i = 0; i < 6; i++) {
					mac[i] = (unsigned char) b[i];
				}
				ret = wpa_port_connect_bssid(argv[2],
				    argv[3], mac);
			} else {
				csh_printf(csh, "wpa: bad bssid %s\r\n",
					   argv[4]);
				return 1;
			}
		} else {
			ret = wpa_port_connect(argv[2], argv[3]);
		}
		if (ret == 0) {
			csh_printf(csh, "wpa: connecting to \"%s\"\r\n",
				   argv[2]);
		} else {
			csh_printf(csh, "wpa: connect failed (%d)\r\n", ret);
		}
		return 0;
	}
	if (strcmp(argv[1], "disconnect") == 0) {
		ret = wpa_port_disconnect();
		csh_printf(csh, "wpa: disconnect %s\r\n",
			   ret == 0 ? "ok" : "failed/busy");
		return 0;
	}

	usage(csh);
	return 0;
}

/* sscanf sits behind newlib's scanf machinery the image does not ship;
 * a MAC address is six hex bytes, parse it by hand */
static int sscanf_bssid(const char *s, unsigned int b[6]) {
	int i;

	for (i = 0; i < 6; i++) {
		unsigned int v = 0;
		int n;

		for (n = 0; n < 2; n++) {
			char c = *s++;

			if (c >= '0' && c <= '9') {
				v = (v << 4) | (unsigned int) (c - '0');
			} else if (c >= 'a' && c <= 'f') {
				v = (v << 4) | (unsigned int) (c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				v = (v << 4) | (unsigned int) (c - 'A' + 10);
			} else {
				return i;
			}
		}
		b[i] = v;
		if (i < 5) {
			if (*s != ':') {
				return i + 1;
			}
			s++;
		}
	}
	return 6;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_wpa, wpa, "wpa start/connect/status/disconnect",
			  "wpa_supplicant control (WPA2-PSK)");
