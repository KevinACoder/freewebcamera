/*
 * @file
 * @brief UAC audio(4) consumer shim entry points.
 *
 * @date 27.09.2026
 * @author zhugengyu
 */

#ifndef FREEWEBCAMERA_AV_AUDIO_H
#define FREEWEBCAMERA_AV_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open unit 0 through the middle layer (idempotent).  Returns 0 or a
 * negative errno. */
int av_audio_open(int unit);

/* Negotiate the capture format (S16_LE).  Call before the first read;
 * the first call opens the unit when it is not open yet. */
int av_audio_set_format(int unit, uint32_t rate, uint32_t channels);

/* One nonblocking read through the fileops table: returns 0 and reports
 * bytes moved plus the errno the middle layer returned (EWOULDBLOCK is
 * the empty-ring answer, and a partial read ends with it too). */
int av_audio_read(int unit, void *buf, size_t len, size_t *out,
    int *errp);

/* Spawn the capture worker (single owner of the read side) and start the
 * stream.  Returns 0 or a negative errno. */
int av_audio_capture_start(int unit);
void av_audio_capture_stop(void);
int av_audio_is_running(void);

void av_audio_info_dump(void);
void av_audio_stats_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_AV_AUDIO_H */
