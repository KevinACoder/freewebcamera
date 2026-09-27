/*
 * @file
 * @brief filio(4) shell: the generic descriptor ioctl commands.
 *
 * audio(4) switches on FIONREAD in its ioctl path (and the fd layer's
 * fcntl handling names FIONBIO/FIOASYNC).  Values are upstream's, so a
 * command built by userland tooling would match; the port consumer uses
 * them only for its own FIONREAD/GETINFO calls.
 */

#ifndef _COMPAT_SYS_FILIO_H_
#define _COMPAT_SYS_FILIO_H_

#include <sys/ioccom.h>

#define FIOCLEX		 _IO('f', 1)
#define FIONCLEX	 _IO('f', 2)
#define FIONREAD	_IOR('f', 127, int)
#define FIONBIO		_IOW('f', 126, int)
#define FIOASYNC	_IOW('f', 125, int)
#define FIOSETOWN	_IOW('f', 124, int)
#define FIOGETOWN	_IOR('f', 123, int)

#endif /* _COMPAT_SYS_FILIO_H_ */
