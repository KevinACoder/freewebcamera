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

/* the system heap and the EHCI interrupt counter: the two numbers that
 * tell "the stream starves the rest of the system" apart from "the
 * stream is fine and something else is wrong" */
extern size_t xPortGetFreeHeapSize(void);
extern volatile unsigned usb_ehci_irq_count;

static int
av_v_ioctl(int unit, u_long cmd, void *arg)
{
	return videoioctl((dev_t) (unsigned) unit, cmd, arg, 0, NULL);
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

	if (av_video_device(unit) == NULL) {
		return -ENXIO;
	}
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	return av_v_ioctl(unit, VIDIOC_S_FMT, &fmt);
}

int
av_video_set_framerate(int unit, uint32_t fps)
{
	struct v4l2_streamparm parm;

	if (av_video_device(unit) == NULL || fps == 0) {
		return -EINVAL;
	}
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	return av_v_ioctl(unit, VIDIOC_S_PARM, &parm);
}

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
	if (err != 0) {
		av_v_read_err++;
		return -err;
	}
	return 0;
}

/* ------------------------------------------------------------------
 * capture worker: drain frames into the counters until stopped; the
 * dump channel (av_dump.c) consumes from the same hook in a later
 * round, this thread is the heartbeat that proves the isoc pipe lives
 */

#define AV_VIDEO_TICK_MS	1000

static void
av_video_worker(void *arg)
{
	static uint8_t frame[AV_VIDEO_FRAME_MAX]; /* one frame in flight */
	int unit = (int) (uintptr_t) arg;
	unsigned last_frames = 0;

	av_v_running = 1;
	while (av_v_running) {
		size_t got = 0;
		int err = av_video_read(unit, frame, sizeof(frame), &got);

		if (err == 0 && got > 0) {
			av_v_frames++;
			av_v_bytes_low += (unsigned) got;
		} else if (err != 0 && err != -EAGAIN) {
			osDelay(AV_VIDEO_TICK_MS);
		}
		(void) last_frames;
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
		printf("avmon frames=%u rderr=%u running=%u opened=%d "
		    "heap=%u ehci_irq=%u shell_wakes=%u rx_down=%d "
		    "uart_isr=%u uart_bytes=%u ev_calls=%u ev_bytes=%u "
		    "sh_read=%u\n",
		    av_v_frames, av_v_read_err, av_v_running, av_v_opened[0],
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
	osThreadId_t tid;

	if (av_v_running) {
		return 0;
	}
	if (av_video_ensure_open(unit) != 0) {
		return -EIO;
	}
	osThreadAttr_t attr = { .name = "av_video", .stack_size = 4096,
	    .priority = osPriorityBelowNormal };
	tid = osThreadNew(av_video_worker, (void *) (uintptr_t) unit, &attr);
	return tid != NULL ? 0 : -EIO;
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
}
