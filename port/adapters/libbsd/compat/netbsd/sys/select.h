/*
 * @file
 * @brief select(9)/poll(9) side shells: the usb device node's event
 * fan-out compiles against these; nothing waits on it here.
 */

#ifndef _COMPAT_SYS_SELECT_H_
#define _COMPAT_SYS_SELECT_H_

#include <sys/cdefs.h>
#include <sys/types.h>

struct selinfo {
	int unused;
};

#include <sys/event.h>

struct lwp;
struct knote;

void selrecord(struct lwp *selector, struct selinfo *sip);
void selnotify(struct selinfo *sip, int events, long knoteflags);
void selrecord_knote(struct selinfo *sip, struct knote *kn);
void selremove_knote(struct selinfo *sip, struct knote *kn);

#endif /* _COMPAT_SYS_SELECT_H_ */
