/*
 * @file
 * @brief UVC video(4) consumer shim interface (port/adapters/libbsd).
 *
 * The unit numbers are the video(4) minors; every call maps straight
 * onto the verbatim middle layer's cdev entry points.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#ifndef PORT_ADAPTERS_LIBBSD_AV_VIDEO_H_
#define PORT_ADAPTERS_LIBBSD_AV_VIDEO_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/device.h>

#define AV_VIDEO_UNITS		2
#define AV_VIDEO_FRAME_MAX	(384 * 1024)	/* caps sizeimage selects */

/* NULL when no uvideo stream attached behind that unit */
device_t av_video_device(int unit);

int av_video_open(int unit);
int av_video_close(int unit);
/* open-once: S_FMT's probe/commit needs the streaming interface claimed */
int av_video_ensure_open(int unit);
/* nonblocking open + probe loop: EAGAIN means the middle layer has no
 * completed frame, a byte count means the egress ring had one waiting */
int av_video_open_nonblock(int unit);
int av_video_read_probe(int unit, unsigned count);
int av_video_get_format(int unit, uint32_t *w, uint32_t *h, uint32_t *pixfmt);
int av_video_set_format(int unit, uint32_t w, uint32_t h, uint32_t pixfmt);
int av_video_set_framerate(int unit, uint32_t fps);
int av_video_enum_format(int unit, int idx, uint32_t *pixfmt, char *desc,
	size_t desclen);

/* one videoread() call; blocks on the middle layer's sample cv when the
 * egress ring is empty (read-method starts the isoc transfer on first
 * use).  Returns 0 and *out = bytes moved. */
int av_video_read(int unit, void *buf, size_t len, size_t *out);

int av_video_capture_start(int unit);
void av_video_capture_stop(void);
void av_video_stats_dump(void);

#endif /* PORT_ADAPTERS_LIBBSD_AV_VIDEO_H_ */
