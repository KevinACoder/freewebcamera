/*
 * @file
 * @brief UVC video(4) consumer shim: the port-world face of the verbatim
 * uvideo + video middle layer.
 *
 * The video(4) cdev entry points (videoopen/videoread/videoioctl, global
 * in the verbatim sys/dev/video.c) take a dev_t whose minor is the unit;
 * with no vnode layer the unit number IS the dev_t.  The first read()
 * with the read-method picks the format's sizeimage, allocates the
 * egress ring and starts the isoc transfer - so the consumer thread only
 * ever calls videoread and the stream self-starts, exactly the read(2)
 * shape the native fork validated with this camera.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "cmsis_os2.h"

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/videoio.h>

#include "av_video.h"
#include "av_dump.h"

/* the video(4) cdev switch (verbatim sys/dev/video.c) */
extern int videoopen(dev_t, int, int, struct lwp *);
extern int videoclose(dev_t, int, int, struct lwp *);
extern int videoread(dev_t, struct uio *, int);
extern int videoioctl(dev_t, u_long, void *, int, struct lwp *);
/* the middle layer's cfdriver (bsd_autoconf.c, ioconf.c's role) */
extern struct cfdriver video_cd;

/* capture counters - the bring-up truth when the isoc pipe is new */
static volatile unsigned av_v_frames;
static volatile unsigned av_v_bytes_low;
static volatile unsigned av_v_read_err;
static volatile unsigned av_v_running;
static int av_v_opened[AV_VIDEO_UNITS];

/* the format the shell asked for, and the reference a later round's
 * restart path needs; recorded in the setters below */
static uint32_t av_v_want_w, av_v_want_h, av_v_want_fourcc, av_v_want_fps;
static volatile int av_v_want_set;

/* The BSD-world console sink only comes alive when the wlan line starts
 * (wlan_adapter's wlan_console_ready); the UVC line is not a wlan line,
 * and a capture's prints must not depend on that (2026-09-27: every uvc
 * command looked dead until 'wlan start' had been run once).  The call is
 * idempotent, and the shell path that reaches here has the UART up. */
extern void wlan_console_ready(void);

/* the system heap and the EHCI interrupt counter: the two numbers that
 * tell "the stream starves the rest of the system" apart from "the
 * stream is fine and something else is wrong" */
extern size_t xPortGetFreeHeapSize(void);
extern volatile unsigned usb_ehci_irq_count;

/* FWRITE matters: the middle layer's VIDIOC_S_FMT/S_PARM paths return
 * EPERM without it (video.c video_set_format / video_set_framerate), so a
 * zero flag silently made every format/framerate request a no-op. */
static int
av_v_ioctl(int unit, u_long cmd, void *arg)
{
	return videoioctl((dev_t) (unsigned) unit, cmd, arg,
	    FWRITE | FREAD, NULL);
}

device_t
av_video_device(int unit)
{
	if (unit < 0 || unit >= AV_VIDEO_UNITS) {
		return NULL;
	}
	return device_lookup(&video_cd, unit);
}

int
av_video_open(int unit)
{
	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	return videoopen((dev_t) (unsigned) unit, 0, 0, NULL);
}

/* The UVC probe/commit inside S_FMT runs control transfers on the
 * streaming interface, so it needs the interface claimed - that happens
 * in open.  The shell configures between ensure_open and the reader
 * thread, and the flag keeps a later "on" from double-opening. */
/* Nonblocking open: videoread then returns EAGAIN when the egress ring is
 * empty instead of waiting on the sample cv - the probe path that tells
 * "the middle layer never completed a frame" apart from "the blocking
 * consumer was never woken". */
int
av_video_open_nonblock(int unit)
{
	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	return videoopen((dev_t) (unsigned) unit, O_NONBLOCK, 0, NULL);
}

/* VIDIOC_QUERYBUF per index: the read-method's buffers live in the middle
 * layer's ingress queue, and only their QUEUED flag (V4L2_BUF_FLAG_QUEUED)
 * says so.  This is the window that tells "never queued" apart from
 * "queued and consumed". */
int
av_video_query_buf(int unit, unsigned idx, uint32_t *flags,
    uint32_t *bytesused, uint32_t *length)
{
	struct v4l2_buffer buf;
	int err;

	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	memset(&buf, 0, sizeof(buf));
	buf.index = idx;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	err = av_v_ioctl(unit, VIDIOC_QUERYBUF, &buf);
	if (err != 0) {
		return -err;
	}
	if (flags != NULL) {
		*flags = buf.flags;
	}
	if (bytesused != NULL) {
		*bytesused = buf.bytesused;
	}
	if (length != NULL) {
		*length = buf.length;
	}
	return 0;
}

/* Nonblocking probe: read a bounded number of frames and count what came
 * back.  It must not run while the capture worker owns the stream: the
 * middle layer keeps one vs_flags per stream, and a second open with
 * O_NONBLOCK rewrote the running consumer's blocking mode (plus the open
 * count never came back down).  One consumer at a time; the probe closes
 * what it opened. */
int
av_video_read_probe(int unit, unsigned count)
{
	static uint8_t buf[AV_VIDEO_FRAME_MAX];
	unsigned i, ok = 0, empty = 0, errs = 0;
	unsigned long bytes = 0;

	if (av_video_device(unit) == NULL) {
		printf("uvc: no video%d\n", unit);
		return -1;
	}
	if (av_v_running) {
		printf("uvc: capture is running - 'uvc stats' for counters, "
		    "or 'uvc video off' first\n");
		return -1;
	}
	if (av_video_open_nonblock(unit) != 0) {
		printf("uvc: video%d nonblocking open failed\n", unit);
		return -1;
	}
	for (i = 0; i < count; i++) {
		size_t got = 0;
		int err = av_video_read(unit, buf, sizeof(buf), &got);

		if (err == 0 && got > 0) {
			ok++;
			bytes += got;
			printf("uvc read[%u] %u bytes head=%02x %02x\n", i,
			    (unsigned) got, buf[0], buf[1]);
		} else if (err == -EAGAIN) {
			empty++;
		} else {
			errs++;
			printf("uvc read[%u] err %d\n", i, err);
		}
		osDelay(200);
	}
	av_video_close(unit);
	printf("uvc read: ok=%u empty=%u err=%u bytes=%lu\n", ok, empty,
	    errs, bytes);
	return 0;
}

int
av_video_ensure_open(int unit)
{
	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	if (av_v_opened[unit]) {
		return 0;
	}
	if (av_video_open(unit) != 0) {
		return -EIO;
	}
	av_v_opened[unit] = 1;
	return 0;
}

int
av_video_close(int unit)
{
	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	/* videoclose stops the transfer and releases the hw */
	return videoclose((dev_t) (unsigned) unit, 0, 0, NULL);
}

int
av_video_get_format(int unit, uint32_t *w, uint32_t *h, uint32_t *pixfmt)
{
	struct v4l2_format fmt;
	int err;

	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	err = av_v_ioctl(unit, VIDIOC_G_FMT, &fmt);
	if (err == 0) {
		if (w != NULL) {
			*w = fmt.fmt.pix.width;
		}
		if (h != NULL) {
			*h = fmt.fmt.pix.height;
		}
		if (pixfmt != NULL) {
			*pixfmt = fmt.fmt.pix.pixelformat;
		}
	}
	return err;
}

int
av_video_set_format(int unit, uint32_t w, uint32_t h, uint32_t pixfmt)
{
	struct v4l2_format fmt;
	int err;

	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	err = av_v_ioctl(unit, VIDIOC_S_FMT, &fmt);
	if (err == 0) {
		av_v_want_w = w;
		av_v_want_h = h;
		av_v_want_fourcc = pixfmt;
		av_v_want_set = 1;
	}
	return err;
}

int
av_video_set_framerate(int unit, uint32_t fps)
{
	struct v4l2_streamparm parm;
	int err;

	if (av_video_device(unit) == NULL || fps == 0) {
		return -EINVAL;
	}
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	err = av_v_ioctl(unit, VIDIOC_S_PARM, &parm);
	if (err == 0) {
		av_v_want_fps = fps;
	}
	return err;
}

/* re-apply the shell's format choice */

int
av_video_enum_format(int unit, int idx, uint32_t *pixfmt, char *desc,
    size_t desclen)
{
	struct v4l2_fmtdesc fd;

	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	memset(&fd, 0, sizeof(fd));
	fd.index = (unsigned int) idx;
	fd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (av_v_ioctl(unit, VIDIOC_ENUM_FMT, &fd) != 0) {
		return -EINVAL;
	}
	if (pixfmt != NULL) {
		*pixfmt = fd.pixelformat;
	}
	if (desc != NULL && desclen != 0) {
		size_t n = sizeof(fd.description);

		if (n > desclen - 1) {
			n = desclen - 1;
		}
		memcpy(desc, fd.description, n);
		desc[n] = '\0';
	}
	return 0;
}

int
av_video_read(int unit, void *buf, size_t len, size_t *out)
{
	struct iovec iov;
	struct uio u;
	int err;

	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	if (buf == NULL || len == 0 || len > AV_VIDEO_FRAME_MAX) {
		return -EINVAL;
	}
	iov.iov_base = buf;
	iov.iov_len = len;
	memset(&u, 0, sizeof(u));
	u.uio_iov = &iov;
	u.uio_iovcnt = 1;
	u.uio_resid = len;
	u.uio_offset = 0;
	u.uio_rw = UIO_READ;
	u.uio_segflg = UIO_SYSSPACE;

	err = videoread((dev_t) (unsigned) unit, &u, 0);
	if (out != NULL) {
		*out = len - u.uio_resid;
	}
	if (err != 0 && err != EAGAIN) {
		/* EAGAIN is the polled consumer's normal empty answer, not an
		 * error: counting it would bury the real failures */
		av_v_read_err++;
	}
	return err != 0 ? -err : 0;
}

/* ------------------------------------------------------------------
 * capture worker: the single owner of the read-method consumer side.
 *
 * The middle layer's read method has no timeout, and its ingress queue
 * refills only through a read that consumes a whole sample, so a consumer
 * that stops reading parks the stream in a drop-forever state (every
 * buffer in egress, ingress empty, vs_drop latched - the MI-layer
 * forensics of the previous round).  Two consequences shape this thread:
 * it polls instead of blocking (a blocking read cannot be interrupted by
 * a stall check), and it owns the recovery ladder, because nothing in the
 * middle layer can restart the stream.
 *
 * Recovery ladder - the two actions a native consumer has, minus the one
 * this port cannot yet do safely:
 *   tier 1  VIDIOC_S_FMT with the current format re-runs the UVC
 *           probe/commit handshake without touching the isoc pipe (the
 *           driver's own frame watchdog takes exactly this action).
 *   tier 2  close/reopen the cdev (stream off, buffers torn down, method
 *           reset, uvideo_open's probe/commit on the way back in) is NOT
 *           automatic: on this port the cdev teardown of a live stream
 *           corrupted the ThreadX heap (2026-09-27 board evidence: fault
 *           in tlsf remove_free_block 68 ms after the close/open started;
 *           the middle layer's scatter pages are freed while the isoc
 *           completion path can still be inside a payload copy).  That is
 *           a fix/uvc-teardown-race line, not something a stalled stream
 *           should risk.  So the ladder stops after tier 1 and says what
 *           the operator can do: the camera's per-boot bad state (KI-044)
 *           needs a cold boot either way.
 * The per-tier counter resets whenever a frame arrives.
 */

#define AV_VIDEO_POLL_MS	2
#define AV_VIDEO_STALL_MS	3000
#define AV_VIDEO_REARM_MAX	2
#define AV_VIDEO_RESET_MAX	3

static volatile unsigned av_v_rearm;
static volatile unsigned av_v_resets;
static volatile unsigned av_v_rearm_total;
static volatile unsigned av_v_resets_total;

static void
av_video_recover(int unit)
{
	uint32_t w = 0, h = 0, fcc = 0;

	if (av_v_rearm < AV_VIDEO_REARM_MAX) {
		av_v_rearm++;
		av_v_rearm_total++;
		printf("uvc: no frame for %ums - re-arming via S_FMT "
		    "(probe/commit, attempt %u/%u)\n", AV_VIDEO_STALL_MS,
		    av_v_rearm, AV_VIDEO_REARM_MAX);
		if (av_video_get_format(unit, &w, &h, &fcc) == 0 && w != 0) {
			(void) av_video_set_format(unit, w, h, fcc);
		}
		return;
	}
	if (av_v_resets < AV_VIDEO_RESET_MAX) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		int err;

		av_v_resets++;
		av_v_resets_total++;
		printf("uvc: still no frame - restarting the isoc pipe "
		    "(STREAMOFF/ON, attempt %u/%u)\n", av_v_resets,
		    AV_VIDEO_RESET_MAX);
		/* the pipe restart keeps the middle layer's buffers: the
		 * driver aborts and re-arms its own xfers (its teardown is
		 * guarded) and uvideo_stream_start_xfer re-runs the camera
		 * handshake with fresh watchdog counters */
		err = av_v_ioctl(unit, VIDIOC_STREAMOFF, &type);
		osDelay(50);
		err |= av_v_ioctl(unit, VIDIOC_STREAMON, &type);
		if (err != 0) {
			printf("uvc: pipe restart failed (%d)\n", err);
		}
	}
}

static void
av_video_worker(void *arg)
{
	static uint8_t frame[AV_VIDEO_FRAME_MAX]; /* one frame in flight */
	int unit = (int) (uintptr_t) arg;
	unsigned stall_ms = 0;
	int gave_up = 0;

	av_v_running = 1;
	while (av_v_running) {
		size_t got = 0;
		int err = av_video_read(unit, frame, sizeof(frame), &got);

		if (err == 0 && got > 0) {
			av_v_frames++;
			av_v_bytes_low += (unsigned) got;
			av_dump_put_video(frame, got);
			stall_ms = 0;
			av_v_rearm = 0;
			av_v_resets = 0;
			gave_up = 0;
			continue;
		}
		/* EAGAIN (empty ring) and a middle-layer error both just mean
		 * "no sample this round": the stall timer owns recovery */
		osDelay(AV_VIDEO_POLL_MS);
		stall_ms += AV_VIDEO_POLL_MS;
		if (stall_ms < AV_VIDEO_STALL_MS) {
			continue;
		}
		stall_ms = 0;
		if (av_v_rearm < AV_VIDEO_REARM_MAX ||
		    av_v_resets < AV_VIDEO_RESET_MAX) {
			av_video_recover(unit);
		} else if (!gave_up) {
			gave_up = 1;
			printf("uvc: stream still silent after %u re-arms and "
			    "%u pipe restarts - the camera is in its per-boot "
			    "bad state (KI-044); cold-boot the board (capture "
			    "stays armed)\n", av_v_rearm_total,
			    av_v_resets_total);
		}
	}
	av_video_close(unit);
	av_v_opened[unit] = 0;
}

/* ------------------------------------------------------------------
 * console/stream health monitor: a low-rate line that keeps reporting
 * while the console INPUT path may be dead (the uvc streaming rounds
 * showed RX dying mid-stream while TX kept flowing).  The uart probe is
 * the one-line console-path snapshot (GIC pend/act + ier/lsr/iir), the
 * counter half says whether the shell task still wakes.
 */

extern void uart_console_line_probe(const char *tag);
extern volatile unsigned shell_task_wakes;
extern volatile unsigned shell_event_calls;
extern volatile unsigned shell_event_bytes;
extern volatile unsigned shell_read_bytes;
extern int uart_console_rx_down(void);
extern volatile unsigned uart_rx_isr_entries;
extern volatile unsigned uart_rx_bytes_seen;

#define AV_MON_TICK_MS	10000
static volatile unsigned av_mon_running;

static void
av_monitor_worker(void *arg)
{
	(void) arg;
	for (;;) {
		osDelay(AV_MON_TICK_MS);
		uart_console_line_probe("avmon");
		printf("avmon frames=%u rderr=%u rearm=%u reset=%u "
		    "running=%u opened=%d heap=%u ehci_irq=%u shell_wakes=%u "
		    "rx_down=%d uart_isr=%u uart_bytes=%u ev_calls=%u "
		    "ev_bytes=%u sh_read=%u\n",
		    av_v_frames, av_v_read_err, av_v_rearm_total,
		    av_v_resets_total, av_v_running, av_v_opened[0],
		    (unsigned) xPortGetFreeHeapSize(), usb_ehci_irq_count,
		    shell_task_wakes, uart_console_rx_down(),
		    uart_rx_isr_entries, uart_rx_bytes_seen,
		    shell_event_calls, shell_event_bytes, shell_read_bytes);
	}
}

static void
av_monitor_start(void)
{
	osThreadAttr_t attr = { .name = "av_mon", .stack_size = 2048,
	    .priority = osPriorityLow };

	if (av_mon_running) {
		return;
	}
	av_mon_running = 1;
	(void) osThreadNew(av_monitor_worker, NULL, &attr);
}

int
av_video_capture_start(int unit)
{
	av_monitor_start();
	wlan_console_ready();
	osThreadId_t tid;

	if (av_v_running) {
		return 0;
	}
	/* the worker is the single consumer and polls, so it needs the
	 * nonblocking open: close the shell's configure session first
	 * (one open count, one vs_flags - both are per stream) */
	if (av_v_opened[unit]) {
		av_video_close(unit);
		av_v_opened[unit] = 0;
	}
	if (av_video_open_nonblock(unit) != 0) {
		printf("uvc: video%d nonblocking open failed\n", unit);
		return -EIO;
	}
	av_v_opened[unit] = 1;
	osThreadAttr_t attr = { .name = "av_video", .stack_size = 4096,
	    .priority = osPriorityBelowNormal };
	tid = osThreadNew(av_video_worker, (void *) (uintptr_t) unit, &attr);
	if (tid == NULL) {
		av_video_close(unit);
		av_v_opened[unit] = 0;
		return -EIO;
	}
	return 0;
}

void
av_video_capture_stop(void)
{
	av_v_running = 0;
}

void
av_video_stats_dump(void)
{
	printf("uvc video frames=%u bytes=%u.%02uMB rderr=%u running=%u "
	    "opened=%d heap=%u ehci_irq=%u\n",
	    av_v_frames, av_v_bytes_low >> 20,
	    ((av_v_bytes_low & 0xfffff) * 100) >> 20, av_v_read_err,
	    av_v_running, av_v_opened[0], (unsigned) xPortGetFreeHeapSize(),
	    usb_ehci_irq_count);
	printf("uvc video rearm=%u reset=%u\n", av_v_rearm_total,
	    av_v_resets_total);
	av_dump_stats_dump();
}
