/*
 * @file
 * @brief poll(2) vocabulary (the usb device node exposes POLLIN).
 */

#ifndef _COMPAT_SYS_POLL_H_
#define _COMPAT_SYS_POLL_H_

#define POLLIN		0x0001
#define POLLRDNORM	0x0040
#define POLLWRNORM	0x0100
#define POLLOUT		0x0004
#define POLLHUP		0x0010

struct pollfd {
	int fd;
	short events;
	short revents;
};

#endif /* _COMPAT_SYS_POLL_H_ */
