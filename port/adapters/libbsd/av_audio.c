/*
 * @file
 * @brief UAC audio(4) consumer shim: the port-world face of the verbatim
 * uaudio + audio(4) middle layer.
 *
 * audio(4) is not a cdev-open device the way video(4) is: its open path
 * fabricates a struct file of its own (fd_allocfile + fd_clone in the
 * middle layer, EMOVEFD on success) and every later operation enters
 * through that file's fileops table.  So this shim opens through the
 * cdevsw d_open the middle layer publishes, picks the resulting file out
 * of the port's file pool by its fileops, and drives read/ioctl/close
 * through the ops table.
 *
 * The consumer shape mirrors av_video.c: one polling owner, a small
 * delay between empty reads, and counters that say what the stream is
 * doing without a scope.
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
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/ioctl.h>
#include <sys/audioio.h>
#include <sys/device.h>
#include <dev/audio/audio_if.h>
#include <sys/uio.h>

#include "av_audio.h"
#include "av_dump.h"

/* the audio(4) cdev entry points (verbatim sys/dev/audio/audio.c): the
 * open is static, so it is reached through the cdevsw it publishes; the
 * fileops table is the global the middle layer registers its files with */
extern const struct cdevsw audio_cdevsw;
extern const struct fileops audio_fileops;
/* the middle layer's cfdriver (bsd_autoconf.c, ioconf.c's role) */
extern struct cfdriver audio_cd;

/* the file pool (bsd_file.c) */
extern struct file *fwc_file_byops(const struct fileops *);
extern unsigned fwc_file_index(const struct file *);

/* BSD-world console sink; see av_video.c for the gate's story */
extern void wlan_console_ready(void);

/* per-channel PCM the consumer drives uaudio with.  The device's mic is
 * a 16-bit PCM input (S16_LE); the rate/channel pair is negotiated at
 * open, preferring 48 kHz stereo and stepping down to what the device
 * accepts. */
#define AV_AUDIO_CHUNK		16384

/* capture counters */
static volatile unsigned av_a_bytes;
static volatile unsigned av_a_reads;
static volatile unsigned av_a_empty;
static volatile unsigned av_a_err;
static volatile unsigned av_a_running;

/* the opened file and the negotiated format */
static struct file *av_a_fp;
static struct audio_info av_a_ai;
static uint32_t av_a_rate;
static uint32_t av_a_channels;
static uint32_t av_a_precision;

static int
av_a_open(int unit)
{
	int err;

	/* AUDIO_DEVICE is the /dev/audio minor tag: AUDIOUNIT() masks the
	 * low nibble, and AUDIODEV() sees the 0x80 that selects the
	 * "initialize every open" semantics (the /dev/sound shape would
	 * inherit the previous session's parameters) */
	dev_t dev = (dev_t) (AUDIO_DEVICE | (unit & 0x0f));

	/* O_NONBLOCK rides in the same flags word the open stores into
	 * f_flag (fd_clone keeps it through FMASK), which is what audioread
	 * tests to pick IO_NDELAY */
	err = audio_cdevsw.d_open(dev, FREAD | O_NONBLOCK, 0, NULL);
	if (err != 0 && err != EMOVEFD) {
		return -err;
	}
	av_a_fp = fwc_file_byops(&audio_fileops);
	if (av_a_fp == NULL) {
		return -EIO;
	}
	return 0;
}

static int
av_a_ioctl(u_long cmd, void *arg)
{
	if (av_a_fp == NULL || av_a_fp->f_ops == NULL ||
	    av_a_fp->f_ops->fo_ioctl == NULL) {
		return -ENXIO;
	}
	return av_a_fp->f_ops->fo_ioctl(av_a_fp, cmd, arg);
}

int
av_audio_set_format(int unit, uint32_t rate, uint32_t channels)
{
	struct audio_info ai;
	int err;

	if (av_a_fp == NULL) {
		err = av_a_open(unit);
		if (err != 0) {
			return err;
		}
	}
	memset(&ai, 0, sizeof(ai));
	AUDIO_INITINFO(&ai);
	ai.record.sample_rate = rate;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.precision = 16;
	ai.record.channels = channels;
	ai.record.pause = 0;
	err = av_a_ioctl(AUDIO_SETINFO, &ai);
	if (err != 0) {
		return err;
	}
	/* read back what the hardware actually took */
	memset(&ai, 0, sizeof(ai));
	AUDIO_INITINFO(&ai);
	if (av_a_ioctl(AUDIO_GETINFO, &ai) == 0) {
		av_a_ai = ai;
		av_a_rate = ai.record.sample_rate;
		av_a_channels = ai.record.channels;
		av_a_precision = ai.record.precision;
	}
	return 0;
}

int
av_audio_open(int unit)
{
	return av_a_open(unit);
}

int
av_audio_read(int unit, void *buf, size_t len, size_t *out, int *errp)
{
	struct iovec iov;
	struct uio u;
	int err;

	(void) unit;
	if (av_a_fp == NULL) {
		return -ENXIO;
	}
	if (buf == NULL || len == 0) {
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

	err = av_a_fp->f_ops->fo_read(av_a_fp, NULL, &u,
	    av_a_fp->f_cred, 0);
	if (out != NULL) {
		/* a partial read ends in EWOULDBLOCK with bytes moved: the
		 * resid is the truth, the error is the shape read(2) gives */
		*out = len - u.uio_resid;
	}
	if (errp != NULL) {
		*errp = err;
	}
	return 0;
}

void
av_audio_info_dump(void)
{
	if (av_a_fp == NULL) {
		printf("uvc audio: not open (run 'uvc audio on')\n");
		return;
	}
	printf("uvc audio info: rate=%u channels=%u precision=%u "
	    "encoding=%u\n", av_a_rate, av_a_channels, av_a_precision,
	    av_a_ai.record.encoding);
	printf("uvc audio: blocksize=%u hiwat=%u lowat=%u buffer_size=%u\n",
	    av_a_ai.blocksize, av_a_ai.hiwat, av_a_ai.lowat,
	    av_a_ai.record.buffer_size);
}

/* ------------------------------------------------------------------
 * capture worker: the single owner of the fileops read side.
 */

#define AV_AUDIO_POLL_MS	2
#define AV_AUDIO_STALL_MS	3000

static void
av_audio_worker(void *arg)
{
	static uint8_t pcm[AV_AUDIO_CHUNK];
	int unit = (int) (uintptr_t) arg;
	unsigned stall_ms = 0;

	av_a_running = 1;
	while (av_a_running) {
		size_t got = 0;
		int err = 0;

		(void) av_audio_read(unit, pcm, sizeof(pcm), &got, &err);
		if (got > 0) {
			av_a_bytes += (unsigned) got;
			av_a_reads++;
			av_dump_put(AV_DUMP_AUDIO, pcm, got);
			stall_ms = 0;
			continue;
		}
		if (err != 0 && err != EWOULDBLOCK && err != EAGAIN) {
			if (av_a_err++ == 0) {
				printf("uvc audio: read error %d\n", err);
			}
		} else {
			av_a_empty++;
		}
		osDelay(AV_AUDIO_POLL_MS);
		stall_ms += AV_AUDIO_POLL_MS;
		if (stall_ms == AV_AUDIO_STALL_MS) {
			printf("uvc audio: no samples for %ums (rate=%u ch=%u)\n",
			    AV_AUDIO_STALL_MS, av_a_rate, av_a_channels);
		}
	}
}

int
av_audio_capture_start(int unit)
{
	/* designated-initializer form on purpose: the remaining CMSIS fields
	 * (stack_mem, cb_mem, attr_bits, ...) must be zero, or osThreadNew
	 * reads garbage and fails (the first bring-up did exactly that) */
	osThreadAttr_t attr = {
		.name = "av_audio",
		.stack_size = 4096,
		.priority = osPriorityBelowNormal,
	};
	osThreadId_t tid;
	int err;

	wlan_console_ready();
	if (av_a_running) {
		return 0;
	}
	if (av_a_fp == NULL) {
		err = av_a_open(unit);
		if (err != 0) {
			printf("uvc audio: audio%d open failed (%d)\n", unit, err);
			return err;
		}
	}
	/* 48 kHz stereo is the microphone's high-quality pair; if the device
	 * rejects it, step down through the usual UAC rates until one takes.
	 * The middle layer resamples as needed (its converter chain is the
	 * linear/mulaw/alaw set), so a rejected SETINFO comes back as the
	 * closest supported format on the GETINFO readback. */
	err = av_audio_set_format(unit, 48000, 2);
	if (err != 0) {
		err = av_audio_set_format(unit, 48000, 1);
	}
	if (err != 0) {
		err = av_audio_set_format(unit, 16000, 1);
	}
	if (err != 0) {
		err = av_audio_set_format(unit, 8000, 1);
	}
	if (err != 0) {
		printf("uvc audio: no supported capture format (%d)\n", err);
		return err;
	}
	tid = osThreadNew(av_audio_worker, (void *) (uintptr_t) unit, &attr);
	if (tid == NULL) {
		printf("uvc audio: capture thread failed to start\n");
		return -EIO;
	}
	printf("uvc audio: capture running (rate=%u ch=%u prec=%u)\n",
	    av_a_rate, av_a_channels, av_a_precision);
	return 0;
}

void
av_audio_capture_stop(void)
{
	av_a_running = 0;
}

int
av_audio_is_running(void)
{
	return av_a_running;
}

void
av_audio_stats_dump(void)
{
	printf("uvc audio bytes=%u reads=%u empty=%u err=%u running=%u "
	    "rate=%u ch=%u prec=%u\n", av_a_bytes, av_a_reads, av_a_empty,
	    av_a_err, av_a_running, av_a_rate, av_a_channels,
	    av_a_precision);
}
