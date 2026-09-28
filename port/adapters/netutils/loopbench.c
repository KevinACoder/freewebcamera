/*
 * @file
 * @brief loop: board-internal throughput bench for the AP + station
 * closed loop.
 *
 * The board's own radios peer inside ONE subnet, so the loop needs a
 * sender bound to the station slot's address and a receiver bound to
 * the AP slot's address - lwIP's source-address routing then keeps the
 * traffic on the air instead of looping it internally.  iperf3's wire
 * protocol is not something to reimplement for that: this is the
 * minimal TCP stream pair (a sink and a sender), reporting bytes,
 * seconds and Mbit/s through the diag sink so a 600 s soak leaves a
 * timestamped trail.  iperf3 (the vendored client) stays the tool for
 * measurements against hosts.
 *
 *   loop recv <bindip> [port] [report_sec]   receiver on the AP side
 *   loop send <bindip> <dstip> [sec] [port]  sender on the station side
 *   loop status | stop
 *
 * @author zhugengyu
 * @date 28.09.2026
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "board.h"

#include "cherrysh_adapter.h"
#include "csh.h"

#define LOOP_PORT_DEFAULT 5290
#define LOOP_BUF 4096

/* static staging: the threads run on 4K stacks, a LOOP_BUF on the C
 * stack overflows instantly (the thread-stack check caught it on the
 * first run) */
static uint8_t loop_rx_buf[LOOP_BUF];
static uint8_t loop_tx_buf[LOOP_BUF];

/* one bench at a time per direction; the threads are joinable only by
 * observation (flags), matching the iperf3 client's shape */
static volatile int loop_rx_run;
static volatile unsigned long loop_rx_bytes;
static volatile int loop_tx_run;
static volatile unsigned long loop_tx_bytes;

static uint32_t loop_secs(void) {
	return (uint32_t) (osKernelGetTickCount() / osKernelGetTickFreq());
}

static void loop_thread(void *arg);

struct loop_cfg {
	int recv;
	ip4_addr_t bind_ip;
	ip4_addr_t dst_ip;
	int port;
	int secs;
};

static int loop_start(const struct loop_cfg *cfg) {
	static osThreadAttr_t attr;
	osThreadId_t id;

	memset(&attr, 0, sizeof(attr));
	attr.name = cfg->recv ? "loop-rx" : "loop-tx";
	attr.stack_size = 4096;
	/* same band as the tcpip peers: above the drivers, below the shell */
	attr.priority = osPriorityAboveNormal;

	id = osThreadNew(loop_thread, (void *) cfg, &attr);
	return id != NULL ? 0 : -1;
}

static void loop_rx_body(const struct loop_cfg *cfg) {
	int srv, cli = -1;
	struct sockaddr_in sin;
	uint8_t *buf = loop_rx_buf;
	uint32_t start;
	int one = 1;

	srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		board_log("loop: recv socket failed\n");
		return;
	}
	(void) setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((u16_t) cfg->port);
	sin.sin_addr.s_addr = cfg->bind_ip.addr;
	if (bind(srv, (struct sockaddr *) &sin, sizeof(sin)) != 0) {
		board_log("loop: recv bind %s failed\n",
		    inet_ntoa(sin.sin_addr));
		closesocket(srv);
		return;
	}
	if (listen(srv, 1) != 0) {
		board_log("loop: recv listen failed\n");
		closesocket(srv);
		return;
	}
	board_log("loop: recv listening %s:%d\n",
	    inet_ntoa(sin.sin_addr), cfg->port);

	/* bounded accept: `loop stop` must be able to tear the bench down */
	{
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

		(void) setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv,
		    sizeof(tv));
	}
	while (loop_rx_run && cli < 0) {
		cli = accept(srv, NULL, NULL);
	}
	closesocket(srv);
	if (cli < 0) {
		board_log("loop: recv stopped (no peer)\n");
		return;
	}
	(void) setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	start = loop_secs();
	for (;;) {
		int n = recv(cli, buf, LOOP_BUF, 0);

		if (n > 0) {
			loop_rx_bytes += (unsigned long) n;
			continue;
		}
		if (!loop_rx_run || n == 0 ||
		    (n < 0 && errno != EWOULDBLOCK)) {
			break; /* stop, peer closed or hard error */
		}
	}
	{
		uint32_t now = loop_secs();
		uint32_t el = now - start;

		board_log("loop: recv done %lu B in %u s (%lu kbit/s)\n",
		    loop_rx_bytes, el,
		    el != 0 ? loop_rx_bytes * 8U / el / 1024U : 0UL);
	}
	closesocket(cli);
}

static void loop_tx_body(const struct loop_cfg *cfg) {
	int s;
	struct sockaddr_in sin;
	uint8_t *buf = loop_tx_buf;
	uint32_t start, el;
	int i;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		board_log("loop: send socket failed\n");
		return;
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = cfg->bind_ip.addr;
	sin.sin_port = 0;
	if (bind(s, (struct sockaddr *) &sin, sizeof(sin)) != 0) {
		board_log("loop: send bind %s failed\n",
		    inet_ntoa(sin.sin_addr));
		closesocket(s);
		return;
	}
	sin.sin_addr.s_addr = cfg->dst_ip.addr;
	sin.sin_port = htons((u16_t) cfg->port);
	if (connect(s, (struct sockaddr *) &sin, sizeof(sin)) != 0) {
		board_log("loop: connect %s:%d failed\n",
		    inet_ntoa(sin.sin_addr), cfg->port);
		closesocket(s);
		return;
	}

	/* a non-zero pattern makes truncation visible on the receiver */
	for (i = 0; i < LOOP_BUF; i++) {
		buf[i] = (uint8_t) i;
	}
	start = loop_secs();
	board_log("loop: send started -> %s:%d for %d s\n",
	    inet_ntoa(sin.sin_addr), cfg->port, cfg->secs);
	for (;;) {
		el = loop_secs() - start;
		if (!loop_tx_run || (cfg->secs > 0 && (int) el >= cfg->secs)) {
			break;
		}
		if (send(s, buf, LOOP_BUF, 0) < 0) {
			board_log("loop: send error at %u s\n", el);
			break;
		}
		loop_tx_bytes += LOOP_BUF;
	}
	/* half-close so the receiver reports a clean total */
	(void) shutdown(s, SHUT_WR);
	board_log("loop: send done %lu B in %u s (%lu kbit/s)\n",
	    loop_tx_bytes, el,
	    el != 0 ? loop_tx_bytes * 8U / el / 1024U : 0UL);
	closesocket(s);
}

static void loop_thread(void *arg) {
	const struct loop_cfg *cfg = arg;

	if (cfg->recv) {
		loop_rx_body(cfg);
		loop_rx_run = 0;
	} else {
		loop_tx_body(cfg);
		loop_tx_run = 0;
	}
}

static int cmd_loop(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	static struct loop_cfg cfg;

	if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
		loop_rx_run = 0;
		loop_tx_run = 0;
		csh_printf(csh, "loop: stop requested\n");
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "status") == 0) {
		csh_printf(csh, "loop: rx=%s bytes=%lu tx=%s bytes=%lu\n",
			   loop_rx_run ? "running" : "idle", loop_rx_bytes,
			   loop_tx_run ? "running" : "idle", loop_tx_bytes);
		return 0;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.port = LOOP_PORT_DEFAULT;
	cfg.secs = 10;

	if (argc >= 3 && strcmp(argv[1], "recv") == 0) {
		if (!ip4addr_aton(argv[2], &cfg.bind_ip)) {
			csh_printf(csh, "loop: bad bind ip %s\n", argv[2]);
			return 0;
		}
		if (argc >= 4) {
			cfg.port = atoi(argv[3]);
		}
		if (loop_rx_run) {
			csh_printf(csh, "loop: recv already running\n");
			return 0;
		}
		cfg.recv = 1;
		loop_rx_bytes = 0;
		loop_rx_run = 1;
		if (loop_start(&cfg) != 0) {
			loop_rx_run = 0;
			csh_printf(csh, "loop: recv thread failed\n");
			return 0;
		}
		csh_printf(csh, "loop: recv on %s:%d, reports on console\n",
			   argv[2], cfg.port);
		return 0;
	}
	if (argc >= 4 && strcmp(argv[1], "send") == 0) {
		if (!ip4addr_aton(argv[2], &cfg.bind_ip) ||
		    !ip4addr_aton(argv[3], &cfg.dst_ip)) {
			csh_printf(csh, "loop: bad address\n");
			return 0;
		}
		if (argc >= 5) {
			cfg.secs = atoi(argv[4]);
		}
		if (argc >= 6) {
			cfg.port = atoi(argv[5]);
		}
		if (loop_tx_run) {
			csh_printf(csh, "loop: send already running\n");
			return 0;
		}
		loop_tx_bytes = 0;
		loop_tx_run = 1;
		if (loop_start(&cfg) != 0) {
			loop_tx_run = 0;
			csh_printf(csh, "loop: send thread failed\n");
			return 0;
		}
		csh_printf(csh, "loop: send %s -> %s:%d for %d s\n",
			   argv[2], argv[3], cfg.port, cfg.secs);
		return 0;
	}

	csh_printf(csh, "usage: loop recv <bindip> [port] | "
		   "loop send <bindip> <dstip> [sec] [port] | status | stop\n");
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_loop, loop, "loop recv/send: board-internal AP<->STA bench",
			  "closed-loop TCP bench bound to slot addresses");
