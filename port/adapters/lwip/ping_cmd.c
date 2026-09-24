/*
 * @file
 * @brief ping command: thin cherrysh front end for the vendored lwIP
 * apps/ping (third-party/lwip/apps/ping - raw mode, PING_USE_SOCKETS=0
 * in this image). All echo logic lives in the vendored app.
 *
 * @author zhugengyu
 * @date 22.09.2026
 */

#include <stdlib.h>
#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

#include "ping.h"

#include "cherrysh_adapter.h"
#include "csh.h"

static int cmd_ping(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	ip_addr_t target;
	u32_t count = 4;

	if (argc < 2) {
		csh_printf(csh, "usage: ping <ip> [count]\r\n");
		return 0;
	}
	if (argc > 2) {
		count = (u32_t) atoi(argv[2]);
	}
	if (count == 0U || count > 32U) {
		count = 4;
	}
	memset(&target, 0, sizeof(target));
	if (!ip4addr_aton(argv[1], ip_2_ip4(&target))) {
		csh_printf(csh, "ping: bad address %s\r\n", argv[1]);
		return 0;
	}
	IP_SET_TYPE_VAL(target, IPADDR_TYPE_V4);
	ping_init(&target, count, NULL);
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_ping, ping, "ping <ip> [count]",
			  "ICMP echo - vendored lwIP apps/ping raw mode");
