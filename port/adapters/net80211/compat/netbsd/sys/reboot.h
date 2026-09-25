/*
 * @file
 * @brief reboot(2) reason bits: ehci's shutdown hook encodes them; no
 * reboot path exists on this carrier.
 */

#ifndef _COMPAT_SYS_REBOOT_H_
#define _COMPAT_SYS_REBOOT_H_

#define RB_AUTOBOOT	0
#define RB_USERBOOT	0x200
#define RB_ASKNAME	0x01
#define RB_SINGLE	0x08

extern int boothowto;

#endif /* _COMPAT_SYS_REBOOT_H_ */
