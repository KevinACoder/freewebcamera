/*
 * @file   ntp_port.c
 * @brief  First-party SNTP client + wall-clock commands for the netutils line.
 *
 * Upstream's netutils ntp/ was NOT vendored: its ntp.h carries a legacy
 * GPL-2 header (early RT-Thread licensing), and the clean-room constraint
 * admits only BSD/MIT/Apache/ISC or first-party code. SNTP itself is tiny
 * and fully specified (RFC 4330): a 48-byte UDP request, mode=3 client, and
 * the 64-bit transmit timestamp's seconds half lands at offset 40. This
 * file is that client, first-party, on the lwIP socket surface.
 *
 * There is no RTC on this trunk (the periph line owns it when it returns),
 * so the synced time lives in a RAM epoch anchored at the tick
 * (netutils_ntp_set_epoch in netutils_shim.c). `ntp_sync` pulls and prints;
 * `date` renders the epoch. Everything is UTC; timezone handling belongs to
 * whoever eventually cares.
 *
 * Commands:
 *   ntp_sync [host]   query (default: the built-in server list), store epoch
 *   date              render the stored epoch as UTC
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "csh.h"
#include "cherrysh_adapter.h"

#define NTP_EPOCH_DELTA		2208988800u	/* 1900 -> 1970, seconds */
#define NTP_PACKET_SIZE		48
#define NTP_UDP_PORT		123
#define NTP_RECV_TIMEOUT_S	5

void netutils_ntp_set_epoch(long unix_sec);
long netutils_ntp_get_epoch(void);

/* --- SNTP query -------------------------------------------------------------- */

static long sntp_query(chry_shell_t *csh, const char *host)
{
	struct addrinfo hint;
	struct addrinfo *res = NULL;
	struct sockaddr_in srv;
	int sock;
	int n;
	uint8_t pkt[NTP_PACKET_SIZE] = {0};
	uint32_t tx_sec;
	long unix_time = 0;
	struct timeval to = {NTP_RECV_TIMEOUT_S, 0};

	memset(&hint, 0, sizeof(hint));
	hint.ai_family = AF_INET;
	hint.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hint, &res) != 0 || res == NULL) {
		csh_printf(csh, "ntp: resolve failed: %s\n", host);
		return 0;
	}
	memcpy(&srv, res->ai_addr, sizeof(srv));
	srv.sin_family = AF_INET;
	srv.sin_port = htons(NTP_UDP_PORT);
	freeaddrinfo(res);

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		csh_printf(csh, "ntp: socket failed\n");
		return 0;
	}
	(void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

	pkt[0] = 0x1b;			/* LI=0, VN=3, Mode=3 (client) */
	{
		int sn = sendto(sock, pkt, sizeof(pkt), 0,
				(struct sockaddr *)&srv, sizeof(srv));

		if (sn < 0) {
			csh_printf(csh, "ntp: send failed: %s (n=%d errno=%d)\n",
				   host, sn, errno);
			lwip_close(sock);
			return 0;
		}
	}

	n = recv(sock, pkt, sizeof(pkt), 0);
	lwip_close(sock);
	if (n < NTP_PACKET_SIZE) {
		csh_printf(csh, "ntp: no reply from %s (%d bytes)\n", host, n);
		return 0;
	}

	memcpy(&tx_sec, &pkt[40], sizeof(tx_sec));	/* Transmit Timestamp s */
	unix_time = (long)(ntohl(tx_sec) - NTP_EPOCH_DELTA);
	return unix_time;
}

/* --- civil calendar rendering (UTC) ------------------------------------------- */

/* Days-from-civil inverse (Hinnant's algorithm): epoch days -> y/m/d. */
static void civil_from_days(long z, long *y, unsigned *m, unsigned *d)
{
	long era, doe, yoe, doy, mp;
	long yy;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	yy = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
	*m = (unsigned)(mp + ((mp < 10) ? 3 : -9));
	*y = yy + ((m <= 2) ? 1 : 0);
}

static void render_utc(long unix_sec, char *buf, int len)
{
	long days = unix_sec / 86400;
	long rem = unix_sec % 86400;
	long y;
	unsigned mo, d;
	unsigned h = (unsigned)(rem / 3600);
	unsigned mi = (unsigned)((rem % 3600) / 60);
	unsigned s = (unsigned)(rem % 60);

	civil_from_days(days, &y, &mo, &d);
	(void)snprintf(buf, len, "%04ld-%02u-%02u %02u:%02u:%02u UTC",
		       y, mo, d, h, mi, s);
}

/* --- commands ------------------------------------------------------------------ */

static const char *const ntp_default_servers[] = {
	"ntp.aliyun.com",
	"ntp1.aliyun.com",
	"pool.ntp.org",
};

static int cmd_ntp_sync(int argc, char *argv[])
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	long t = 0;
	unsigned i;

	if (argc > 1) {
		t = sntp_query(csh, argv[1]);
	} else {
		for (i = 0; i < sizeof(ntp_default_servers) /
			     sizeof(ntp_default_servers[0]); i++) {
			t = sntp_query(csh, ntp_default_servers[i]);
			if (t != 0) {
				break;
			}
		}
	}

	if (t == 0) {
		csh_printf(csh, "ntp: sync failed\n");
		return -1;
	}
	netutils_ntp_set_epoch(t);
	{
		char buf[40];

		render_utc(t, buf, sizeof(buf));
		csh_printf(csh, "ntp: %s (epoch %ld)\n", buf, t);
	}
	return 0;
}
CSH_CMD_EXPORT_ALIAS_FULL(cmd_ntp_sync, ntp_sync, "ntp_sync [host]",
			  "sync the wall clock from an NTP server (UTC)");

static int cmd_date(int argc, char *argv[])
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	long t = netutils_ntp_get_epoch();
	char buf[40];

	(void)argc;
	(void)argv;
	if (t == 0) {
		csh_printf(csh, "clock unset - run ntp_sync\n");
		return -1;
	}
	render_utc(t, buf, sizeof(buf));
	csh_printf(csh, "%s (epoch %ld)\n", buf, t);
	return 0;
}
CSH_CMD_EXPORT_ALIAS_FULL(cmd_date, date, "date",
			  "show the synced wall clock (UTC)");
