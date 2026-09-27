/*
 * @file
 * @brief the config(8)-generated audio options header ("opt_audio.h").
 *
 * Upstream's files.audio defines opt_audio.h with the AUDIO_BLK_MS
 * parameter and the AUDIO_DEBUG flag.  Neither is set here: AUDIO_BLK_MS
 * then falls back to audiodef.h's default (10 ms blocks) and AUDIO_DEBUG
 * stays off, which is the same default shape the video(4) import uses
 * (VIDEO_DEBUG present but 0).  Present-but-empty is the point - the
 * quoted include has to resolve under _KERNEL_OPT.
 */

#ifndef _COMPAT_OPT_AUDIO_H_
#define _COMPAT_OPT_AUDIO_H_

#endif /* _COMPAT_OPT_AUDIO_H_ */
