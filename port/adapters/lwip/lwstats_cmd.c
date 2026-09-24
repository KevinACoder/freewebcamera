/*
 * @file   lwstats_cmd.c
 * @brief  Shell command dumping lwIP's internal statistics.
 *
 * stats_display() covers the heap, every memp pool and the per-protocol
 * counters. For a wedged bulk flow take two snapshots ~10 s apart and
 * diff: tcp.recv advancing while tcp.xmit frozen = the ACK/window path
 * stalled; tcp.drop or mem.err climbing = the bridge or the heap eating
 * segments; mem.max pinned at MEM_SIZE = heap exhaustion. This was the
 * missing window during the M11 TCP-downlink chase (LWIP_STATS used to
 * be compiled out entirely).
 *
 * @author zhugengyu
 * @date   23.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "cherrysh_adapter.h"
#include "csh.h"
#include "lwip/stats.h"

static int cmd_lwstats(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	(void)argc;
	(void)argv;

	/* the compile-time shape the running stack was built with - the
	 * advertised receive window (seen on the wire) must stay inside
	 * the radio chip's 16K RX FIFO or bursts overflow it */
	csh_printf(csh, "cfg: MSS=%u TCP_WND=%u SND_BUF=%u PBUF_POOL=%ux%u\r\n",
		   (unsigned)TCP_MSS, (unsigned)TCP_WND, (unsigned)TCP_SND_BUF,
		   (unsigned)PBUF_POOL_SIZE, (unsigned)PBUF_POOL_BUFSIZE);

	stats_display();
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_lwstats, lwstats, "lwstats",
			  "dump lwIP statistics (heap, memp, protocols)");
