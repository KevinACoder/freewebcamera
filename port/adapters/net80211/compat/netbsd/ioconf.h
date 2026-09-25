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

#endif /* _IOCONF_H_ */