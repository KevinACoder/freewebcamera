/*
 * @file
 * @brief Raw A/V byte-stream dump channel: the port's "get the bytes off
 * the board" seam.
 *
 * The task's acceptance is a multimodal read of the *actual* stream, so
 * this channel is deliberately the least processed thing possible: the
 * capture side hands over whole frames, the sender writes them to a TCP
 * socket with no in-band framing of any kind (the host resyncs MJPG by
 * SOI/EOI).
 *
 * Two directions, because the lab path decides which one is reachable:
 *   SERVER  the board listens, the host dials in ("uvc dump listen 9100",
 *           host side: tools/host/av_stream_recv.py -c 192.168.0.249 9100).
 *           No Windows firewall/portproxy rule needed - WSL reaches the
 *           board's LAN address directly, and inbound TCP to the board is
 *           the telnetd/tftp precedent.
 *   CLIENT  the board dials out ("uvc dump 192.168.0.18:9100"), the same
 *           direction the pcap dump takes; that path needs an inbound
 *           allowance on the host for the chosen port.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#ifndef FREEWEBCAMERA_AV_DUMP_H
#define FREEWEBCAMERA_AV_DUMP_H

#include <stddef.h>
#include <stdint.h>

/* channels: 0 = video (mjpg), 1 = audio (reserved for the uaudio line) */
#define AV_DUMP_VIDEO	0
#define AV_DUMP_AUDIO	1
#define AV_DUMP_CHANNELS 2

/* directions */
#define AV_DUMP_SERVER	0	/* board listens; spec = "<port>" */
#define AV_DUMP_CLIENT	1	/* board dials out; spec = "<ip>:<port>" */

/* Start streaming the channel; the connection is made (and re-made) by
 * the sender thread, so either side may come up first.  Returns 0 on
 * accept of the request, -1 on a bad spec or a failed socket/thread. */
int av_dump_start(int chan, int mode, const char *spec);

/* Stop the channel: the sender thread drops the socket and exits. */
void av_dump_stop(int chan);

/* Hand one raw payload to the channel.  Whole frames only (a partial
 * frame is dropped and counted - the ring never splits a frame across a
 * loss); called from the capture worker. */
void av_dump_put(int chan, const void *buf, size_t len);
void av_dump_put_video(const void *buf, size_t len);

int av_dump_running(int chan);
void av_dump_stats_dump(void);

#endif /* FREEWEBCAMERA_AV_DUMP_H */