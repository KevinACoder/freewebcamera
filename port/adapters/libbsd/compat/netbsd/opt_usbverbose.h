/*
 * @file
 * @brief config(8)'s option header for `options USBVERBOSE` - not
 * defined here, exactly as in the lab's generated RK3568 kernel
 * (work/.../compile/RK3568_NFS/opt_usbverbose.h: "option `USBVERBOSE'
 * not defined").  usb_subr.c includes it by name under _KERNEL_OPT; the
 * DPRINTF history is what this port wants, not USBVERBOSE's extra
 * device-level chatter.
 */

#ifndef _OPT_USBVERBOSE_H_
#define _OPT_USBVERBOSE_H_

/* #define USBVERBOSE 1 - deliberately off (matches the reference kernel). */

#endif /* _OPT_USBVERBOSE_H_ */