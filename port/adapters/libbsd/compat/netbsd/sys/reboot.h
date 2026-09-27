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

/* the console-verbosity bits bootverbose tests (upstream values) */
#define AB_VERBOSE	0x00020000
#define AB_QUIET	0x00080000

extern int boothowto;
/* upstream spells bootverbose in <sys/systm.h> off this word, and
 * uaudio.c includes reboot.h "for bootverbose": the bit lives here with
 * the word, so the macro lives here */
#define bootverbose	(boothowto & AB_VERBOSE)

#endif /* _COMPAT_SYS_REBOOT_H_ */
