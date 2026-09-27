/*
 * @file
 * @brief ioctl(4) shell for the BSD world: the command builders plus the
 * generic descriptor commands.
 *
 * Upstream sys/ioctl.h ends in a set of subsystem command headers
 * (ttycom/dkio/filio/sockio); of those only filio(4)'s generic
 * descriptor commands are reachable from the compiled set - audio(4)
 * switches on FIONREAD/FIOASYNC/FIONBIO.  The subsystem headers that
 * need heavier context (ttycom, dkio, sockio) stay out: the files that
 * want sockio include <sys/sockio.h> directly, as upstream's own
 * net80211 does.
 *
 * This shadows the osal-facing stub (osal/compat/sys/ioctl.h) for BSD
 * translation units, matching upstream's include shape more closely.
 */

#ifndef _COMPAT_NETBSD_SYS_IOCTL_H_
#define _COMPAT_NETBSD_SYS_IOCTL_H_

#include <sys/ioccom.h>
#include <sys/filio.h>

#endif /* _COMPAT_NETBSD_SYS_IOCTL_H_ */
