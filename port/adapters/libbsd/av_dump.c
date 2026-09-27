/*
 * @file
 * @brief Raw A/V dump channel implementation: producer = capture worker,
 * consumer = one sender thread per channel, transport = lwIP TCP.
 *
 * Shape notes (they are deliberate):
 *  - the ring is static .bss, not the system heap: the wlan line and the
 *    RX pipeline own the heap, and a video stream that churned it would
 *    be the mbuf-pool mistake again (see that decision in the workspace);
 *  - a whole frame goes in or it is dropped and counted - the host sees a
 *    contiguous byte stream and resyncs MJPG by SOI/EOI, so a drop shows
 *    up as a missing frame, never as a torn one;
 *  - sockets are nonblocking: a stalled peer fills the ring (frames are
 *    dropped and counted) instead of parking the capture side, and a
 *    connection that cannot send for AV_DUMP_STUCK_MS is dropped and
 *    re-made, so either side can be restarted at any time;
 *  - server mode accepts repeatedly: the host reconnecting gets a fresh
 *    connection without touching the board.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cmsis_os2.h"

#include <lwip/sockets.h>

#include "av_dump.h"

/* The build configuration (generated): the per-channel ring size is a key
 * (CONFIG_AV_DUMP_RING_BYTES); the rest are protocol constants. */
#include "config.h"

#define AV_DUMP_RING_SIZE	((unsigned) CONFIG_AV_DUMP_RING_BYTES)
#define AV_DUMP_STAGE		8192u
#define AV_DUMP_POLL_MS		5u
#define AV_DUMP_STUCK_MS	5000u
#define AV_DUMP_RECONN_MS	1000u
#define AV_DUMP_ACCEPT_MS	200u

struct av_dump_chan {
	volatile unsigned	running;
	volatile unsigned	connected;
	volatile unsigned	head;		/* bytes ever enqueued */
	volatile unsigned	tail;		/* bytes ever sent */
	volatile unsigned	frames_in;
	volatile unsigned	tx_bytes;
	volatile unsigned	drop_frames;
	volatile unsigned	conn_lost;
	volatile unsigned	connects;
	int			mode;
	uint8_t			ring[AV_DUMP_RING_SIZE];
	uint8_t			stage[AV_DUMP_STAGE];
	osMutexId_t		lock;
	osSemaphoreId_t		sem;
	char			host[32];
	int			port;
};

static struct av_dump_chan av_dump_ch[AV_DUMP_CHANNELS];

static int
av_dump_init(struct av_dump_chan *ch)
{
	if (ch->lock == NULL) {
		ch->lock = osMutexNew(NULL);
		ch->sem = osSemaphoreNew(0xffff, 0, NULL);
	}
	return (ch->lock != NULL && ch->sem != NULL) ? 0 : -1;
}

static int
av_dump_connect(struct av_dump_chan *ch)
{
	struct sockaddr_in addr;
	int s;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u16_t) ch->port);
	if (inet_aton(ch->host, &addr.sin_addr) == 0 ||
	    connect(s, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		lwip_close(s);
		return -1;
	}
	(void) lwip_fcntl(s, F_SETFL, O_NONBLOCK);
	return s;
}

static int
av_dump_listen(struct av_dump_chan *ch)
{
	struct sockaddr_in addr;
	int s, one = 1;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		return -1;
	}
	(void) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((u16_t) ch->port);
	if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
	    listen(s, 1) != 0) {
		lwip_close(s);
		return -1;
	}
	/* poll the accept: a blocking accept cannot be interrupted by
	 * 'uvc dump off' */
	(void) lwip_fcntl(s, F_SETFL, O_NONBLOCK);
	return s;
}

/*
 * Ship the ring over one connection until it breaks or the channel stops.
 * Returns when the caller should re-establish the connection.
 */
static void
av_dump_serve(struct av_dump_chan *ch, int sock)
{
	unsigned stuck_ms = 0;

	while (ch->running) {
		unsigned used, n, idx, first;
		int rc;

		used = ch->head - ch->tail;
		if (used == 0) {
			(void) osSemaphoreAcquire(ch->sem, 200);
			continue;
		}
		n = used > AV_DUMP_STAGE ? AV_DUMP_STAGE : used;
		idx = ch->tail & (AV_DUMP_RING_SIZE - 1);
		/* peek: producers only append at head, only this thread moves
		 * tail - the window is stable */
		osMutexAcquire(ch->lock, osWaitForever);
		first = AV_DUMP_RING_SIZE - idx;
		if (first > n) {
			first = n;
		}
		memcpy(ch->stage, ch->ring + idx, first);
		if (n > first) {
			memcpy(ch->stage + first, ch->ring, n - first);
		}
		osMutexRelease(ch->lock);

		rc = send(sock, ch->stage, (int) n, 0);
		if (rc > 0) {
			osMutexAcquire(ch->lock, osWaitForever);
			ch->tail += (unsigned) rc;
			osMutexRelease(ch->lock);
			ch->tx_bytes += (unsigned) rc;
			stuck_ms = 0;
			continue;
		}
		/* rc <= 0: peer send buffer full, or the link is gone */
		stuck_ms += AV_DUMP_POLL_MS;
		if (stuck_ms >= AV_DUMP_STUCK_MS) {
			printf("avdump: send stuck %ums, dropping connection "
			    "(tx=%u bytes)\n", AV_DUMP_STUCK_MS,
			    ch->tx_bytes);
			return;
		}
		osDelay(AV_DUMP_POLL_MS);
	}
}

static void
av_dump_sender(void *arg)
{
	struct av_dump_chan *ch = (struct av_dump_chan *) arg;
	int sock = -1;		/* client: the peer; server: the listener */

	while (ch->running) {
		if (sock < 0) {
			if (ch->mode == AV_DUMP_SERVER) {
				sock = av_dump_listen(ch);
				if (sock < 0) {
					printf("avdump: listen on port %d "
					    "failed\n", ch->port);
					ch->running = 0;
					break;
				}
				printf("avdump: listening on port %d "
				    "(host dials in)\n", ch->port);
			} else {
				if (ch->host[0] == '\0') {
					osDelay(AV_DUMP_RECONN_MS);
					continue;
				}
				sock = av_dump_connect(ch);
				if (sock < 0) {
					osDelay(AV_DUMP_RECONN_MS);
					continue;
				}
				ch->connected = 1;
				ch->connects++;
				printf("avdump: %s:%d connected (raw "
				    "stream)\n", ch->host, ch->port);
			}
		}

		if (ch->mode == AV_DUMP_SERVER) {
			int conn = accept(sock, NULL, NULL);

			if (conn < 0) {
				osDelay(AV_DUMP_ACCEPT_MS);
				continue;
			}
			(void) lwip_fcntl(conn, F_SETFL, O_NONBLOCK);
			ch->connected = 1;
			ch->connects++;
			printf("avdump: host connected (raw stream, no "
			    "framing)\n");
			av_dump_serve(ch, conn);
			lwip_close(conn);
			ch->connected = 0;
			ch->conn_lost++;
		} else {
			av_dump_serve(ch, sock);
			if (!ch->running) {
				break;
			}
			lwip_close(sock);
			sock = -1;
			ch->connected = 0;
			ch->conn_lost++;
			osDelay(AV_DUMP_RECONN_MS);
		}
	}

	if (sock >= 0) {
		lwip_close(sock);
	}
	ch->connected = 0;
	printf("avdump: sender stopped (tx=%u bytes, frames=%u, "
	    "dropped=%u, lost=%u)\n", ch->tx_bytes, ch->frames_in,
	    ch->drop_frames, ch->conn_lost);
}

static int
av_dump_parse(struct av_dump_chan *ch, int mode, const char *spec)
{
	if (mode == AV_DUMP_SERVER) {
		ch->host[0] = '\0';
		ch->port = atoi(spec);
	} else {
		const char *colon = strrchr(spec, ':');
		size_t hlen;

		if (colon == NULL || colon == spec) {
			return -1;
		}
		hlen = (size_t) (colon - spec);
		if (hlen >= sizeof(ch->host)) {
			return -1;
		}
		memcpy(ch->host, spec, hlen);
		ch->host[hlen] = '\0';
		ch->port = atoi(colon + 1);
	}
	if (ch->port <= 0 || ch->port > 65535) {
		return -1;
	}
	ch->mode = mode;
	return 0;
}

int
av_dump_start(int chan, int mode, const char *spec)
{
	struct av_dump_chan *ch;
	/* one name per channel: ThreadX keeps names distinct so a thread
	 * list/dump can tell the video sender from the audio one */
	static const char *const av_dump_names[AV_DUMP_CHANNELS] = {
		"av_dump0", "av_dump1",
	};
	osThreadAttr_t attr = {
		.name = "av_dump",
		.stack_size = 4096,
		.priority = osPriorityBelowNormal,
	};

	if (chan < 0 || chan >= AV_DUMP_CHANNELS || spec == NULL ||
	    (mode != AV_DUMP_SERVER && mode != AV_DUMP_CLIENT)) {
		return -1;
	}
	attr.name = av_dump_names[chan];
	ch = &av_dump_ch[chan];
	if (av_dump_init(ch) != 0) {
		printf("avdump: chan%d init failed\n", chan);
		return -1;
	}
	if (ch->running) {
		printf("avdump: chan%d already running - 'uvc dump off' "
		    "first\n", chan);
		return -1;
	}
	if (av_dump_parse(ch, mode, spec) != 0) {
		printf("avdump: bad target '%s' (want <ip>:<port> or a "
		    "port)\n", spec);
		return -1;
	}

	ch->head = 0;
	ch->tail = 0;
	ch->frames_in = 0;
	ch->tx_bytes = 0;
	ch->drop_frames = 0;
	ch->conn_lost = 0;
	ch->connects = 0;
	ch->running = 1;
	if (osThreadNew(av_dump_sender, ch, &attr) == NULL) {
		ch->running = 0;
		printf("avdump: chan%d sender thread failed\n", chan);
		return -1;
	}
	if (ch->mode == AV_DUMP_SERVER) {
		printf("avdump: chan%d server on port %d (raw bytes, no "
		    "framing)\n", chan, ch->port);
	} else {
		printf("avdump: chan%d -> %s:%d (raw bytes, no framing)\n",
		    chan, ch->host, ch->port);
	}
	return 0;
}

void
av_dump_stop(int chan)
{
	struct av_dump_chan *ch;

	if (chan < 0 || chan >= AV_DUMP_CHANNELS) {
		return;
	}
	ch = &av_dump_ch[chan];
	if (!ch->running) {
		return;
	}
	ch->running = 0;
	printf("avdump: chan%d stopping (tx=%u bytes, frames=%u, "
	    "dropped=%u)\n", chan, ch->tx_bytes, ch->frames_in,
	    ch->drop_frames);
}

void
av_dump_put(int chan, const void *buf, size_t len)
{
	struct av_dump_chan *ch;
	unsigned used, idx, first;

	if (chan < 0 || chan >= AV_DUMP_CHANNELS || buf == NULL || len == 0) {
		return;
	}
	ch = &av_dump_ch[chan];
	if (!ch->running || ch->lock == NULL) {
		return;
	}
	if (len > AV_DUMP_RING_SIZE) {
		ch->drop_frames++;
		return;
	}
	osMutexAcquire(ch->lock, osWaitForever);
	used = ch->head - ch->tail;
	if (AV_DUMP_RING_SIZE - used < len) {
		/* a frame goes in whole or not at all */
		ch->drop_frames++;
		osMutexRelease(ch->lock);
		return;
	}
	idx = ch->head & (AV_DUMP_RING_SIZE - 1);
	first = AV_DUMP_RING_SIZE - idx;
	if (first > len) {
		first = (unsigned) len;
	}
	memcpy(ch->ring + idx, buf, first);
	if (len > first) {
		memcpy(ch->ring, (const uint8_t *) buf + first, len - first);
	}
	ch->head += (unsigned) len;
	ch->frames_in++;
	osMutexRelease(ch->lock);
	osSemaphoreRelease(ch->sem);
}

void
av_dump_put_video(const void *buf, size_t len)
{
	av_dump_put(AV_DUMP_VIDEO, buf, len);
}

void
av_dump_put_audio(const void *buf, size_t len)
{
	av_dump_put(AV_DUMP_AUDIO, buf, len);
}

int
av_dump_running(int chan)
{
	if (chan < 0 || chan >= AV_DUMP_CHANNELS) {
		return 0;
	}
	return av_dump_ch[chan].running ? 1 : 0;
}

void
av_dump_stats_dump(void)
{
	int i;

	for (i = 0; i < AV_DUMP_CHANNELS; i++) {
		struct av_dump_chan *ch = &av_dump_ch[i];

		if (!ch->running && ch->connects == 0) {
			continue;
		}
		printf("avdump chan%d %s%s%d conn=%u tx=%u frames=%u "
		    "drop=%u relost=%u queued=%u\n", i,
		    ch->mode == AV_DUMP_SERVER ? "listen:" : "",
		    ch->host, ch->port, ch->connected, ch->tx_bytes,
		    ch->frames_in, ch->drop_frames, ch->conn_lost,
		    ch->head - ch->tail);
	}
}