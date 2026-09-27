/*
 * @file
 * @brief The cfdriver extern declarations config(8)'s ioconf.h would
 * carry; the definitions live in bsd_autoconf.c.
 */

#ifndef _IOCONF_H_
#define _IOCONF_H_

#include <sys/device.h>

extern struct cfdriver usb_cd;
extern struct cfdriver uroothub_cd;
extern struct cfdriver uhub_cd;
extern struct cfdriver ehci_cd;
extern struct cfdriver urtwn_cd;
#if UVC_BUILD
extern struct cfdriver uvideo_cd;
extern struct cfdriver video_cd;
#endif

#endif /* _IOCONF_H_ */