/*
 * @file
 * @brief The cfdriver extern declarations config(8)'s ioconf.h would
 * carry; the definitions live in bsd_autoconf.c.
 */

#ifndef _IOCONF_H_
#define _IOCONF_H_

#include <sys/device.h>

/* The build configuration (generated).  This is a port header (not an
 * upstream one), so reading the keys directly is fine; config.h is a plain
 * macro header with no includes of its own. */
#include "config.h"

#if CONFIG_BUS_USB
extern struct cfdriver usb_cd;
extern struct cfdriver uroothub_cd;
extern struct cfdriver uhub_cd;
extern struct cfdriver ehci_cd;
#endif
#if CONFIG_NIC_URTWN
extern struct cfdriver urtwn_cd;
#endif
#if CONFIG_UVC
extern struct cfdriver uvideo_cd;
extern struct cfdriver video_cd;
#endif
#if CONFIG_UAC
extern struct cfdriver uaudio_cd;
extern struct cfdriver audio_cd;
#endif

#endif /* _IOCONF_H_ */