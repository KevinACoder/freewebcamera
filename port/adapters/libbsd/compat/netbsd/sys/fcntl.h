/*
 * @file
 * @brief open(2) flag vocabulary used by the usb device node paths.
 */

#ifndef _COMPAT_SYS_FCNTL_H_
#define _COMPAT_SYS_FCNTL_H_

#define FREAD		0x00000001
#define FWRITE		0x00000002
#define FNONBLOCK	0x00000004
#define FFLAGS_DEFAULT	0x00000080

#define IO_NDELAY	0x01	/* fcntl flag bits (fcntl(9)) */

/* the open(2) spelling the cdev entry points test (upstream value;
 * unrelated to the FNONBLOCK file-flag bit) */
#ifndef O_NONBLOCK
#define O_NONBLOCK	0x00004000
#endif
#define IO_APPEND	0x02

/* file ioctl commands (usb device node async toggling) */
#ifndef FIONBIO
#define FIONBIO		0x8004667eUL
#endif
#ifndef FIOASYNC
#define FIOASYNC	0x8004667dUL
#endif
#ifndef FIOSETOWN
#define FIOSETOWN	0x8004667cUL
#endif

#endif /* _COMPAT_SYS_FCNTL_H_ */
