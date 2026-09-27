/*
 * @file
 * @brief uvc shell command: enumerate the video(4) units the uvideo
 * driver attached, pick a format and pump the capture worker.
 *
 * Same shape as the wlan command: dispatch in the command body, the
 * console is cherrysh, everything below goes through av_video.h (the
 * video(4) cdev shim).
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#include <stdlib.h>
#include <string.h>

#include "cherrysh_adapter.h"
#include "csh.h"

#include <sys/videoio.h>

#include "av_video.h"

static uint32_t
uvc_fourcc(const char *s)
{
	char c[4] = { 'M', 'J', 'P', 'G' };
	int i;

	if (s != NULL) {
		for (i = 0; i < 4 && s[i] != '\0'; i++) {
			c[i] = s[i];
		}
	}
	return v4l2_fourcc(c[0], c[1], c[2], c[3]);
}

static void
uvc_print_fourcc(uint32_t f, char out[5])
{
	out[0] = (char) (f & 0xff);
	out[1] = (char) ((f >> 8) & 0xff);
	out[2] = (char) ((f >> 16) & 0xff);
	out[3] = (char) ((f >> 24) & 0xff);
	out[4] = '\0';
}

static int
uvc_list(void)
{
	int unit;

	for (unit = 0; unit < AV_VIDEO_UNITS; unit++) {
		char desc[40];
		uint32_t pixfmt;
		uint32_t w, h, fcc;
		char fccs[5];
		int idx;

		if (av_video_device(unit) == NULL) {
			continue;
		}
		printf("uvc: video%d at %s\n", unit,
		    ((device_t) av_video_device(unit))->dv_xname);
		if (av_video_open(unit) != 0) {
			printf("uvc: video%d open failed\n", unit);
			continue;
		}
		for (idx = 0; av_video_enum_format(unit, idx, &pixfmt,
		    desc, sizeof(desc)) == 0; idx++) {
			uvc_print_fourcc(pixfmt, fccs);
			printf("uvc:  fmt[%d] %s (%s)\n", idx, fccs, desc);
		}
		if (av_video_get_format(unit, &w, &h, &fcc) == 0) {
			uvc_print_fourcc(fcc, fccs);
			printf("uvc:  current %ux%u %s\n", w, h, fccs);
		}
		av_video_close(unit);
	}
	printf("uvc: usage: uvc video on [WxH] [FCCC] [fps] | off | stats\n");
	return 0;
}

static void
uvc_capture(int unit, int on, const char *geom, const char *fmt,
    unsigned fps)
{
	if (!on) {
		av_video_capture_stop();
		printf("uvc: capture stopped\n");
		return;
	}
	/* open first: uvideo's S_FMT runs UVC probe/commit control
	 * transfers on the streaming interface, which open claims */
	if (av_video_ensure_open(unit) != 0) {
		printf("uvc: video%d open failed\n", unit);
		return;
	}
	if (geom != NULL) {
		unsigned w = 0, h = 0;
		const char *x = strchr(geom, 'x');
		int err;

		if (x != NULL) {
			w = (unsigned) atoi(geom);
			h = (unsigned) atoi(x + 1);
		}
		if (w == 0 || h == 0) {
			printf("uvc: bad geometry %s (want WxH)\n", geom);
			return;
		}
		err = av_video_set_format(unit, w, h, uvc_fourcc(fmt));
		if (err != 0) {
			printf("uvc: S_FMT %s %s rejected (errno %d)\n",
			    geom, fmt != NULL ? fmt : "MJPG", -err);
			return;
		}
	}
	if (fps != 0 && av_video_set_framerate(unit, fps) != 0) {
		printf("uvc: S_PARM %ufps rejected (continuing)\n", fps);
	}
	if (av_video_capture_start(unit) != 0) {
		printf("uvc: capture start failed\n");
		return;
	}
	printf("uvc: capture running on video%d\n", unit);
}

static int
cmd_uvc(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	(void) csh;

	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		return uvc_list();
	}
	if (argc >= 3 && strcmp(argv[1], "video") == 0) {
		int on = strcmp(argv[2], "on") == 0;

		if (on || strcmp(argv[2], "off") == 0) {
			const char *geom = argc > 3 ? argv[3] : NULL;
			const char *fmt = argc > 4 ? argv[4] : NULL;
			unsigned fps = argc > 5
			    ? (unsigned) atoi(argv[5]) : 0;

			uvc_capture(0, on, geom, fmt, fps);
			return 0;
		}
	}
	if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
		av_video_stats_dump();
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "read") == 0) {
		unsigned count = argc > 2 ? (unsigned) atoi(argv[2]) : 10;

		(void) av_video_read_probe(0, count);
		return 0;
	}
	printf("usage: uvc list | video on [WxH] [FCCC] [fps] | "
	    "video off | stats | read [n]\n");
	return 0;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_uvc, uvc,
    "uvc list | video on [WxH] [FCCC] [fps] | video off | stats",
    "UVC camera: enumerate Pro 9000 formats, pump the isoc stream, "
    "print capture counters");
