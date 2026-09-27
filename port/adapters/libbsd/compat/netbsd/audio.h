/*
 * @file
 * @brief the config(8)-generated device-count header audio(4) includes by
 * quoted name ("audio.h"); NAUDIO=1 plays ioconf.c's role for the UAC
 * line, the same way compat video.h carries NVIDEO.  NMIDI/NMIDIBUS stay
 * 0: the audio line imports the MI audio middle layer only, not midi(4)
 * (audio_if.h refuses to compile when all three are zero, so NAUDIO has
 * to be the one that is set).
 */

#ifndef _COMPAT_AUDIO_H_
#define _COMPAT_AUDIO_H_

#define NAUDIO		1
#define NMIDI		0
#define NMIDIBUS	0

#endif /* _COMPAT_AUDIO_H_ */
