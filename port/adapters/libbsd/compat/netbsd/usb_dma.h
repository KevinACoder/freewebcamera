/*
 * @file
 * @brief config(8)'s per-device count header for `usb_dma: usb` (a
 * needs-flag device, so config generates this file).  usbdi.c includes
 * it by name and writes `#if NUSB_DMA > 0` around the DMA buffer path:
 * undefined, that path compiles out and usbd_alloc_buffer() hands back a
 * plain heap buffer while xfer->ux_dmabuf stays stale - every device
 * transfer then gets a qTD pointing at whatever was in that field, and
 * the root hub still works because its control path is a direct call.
 * That was exactly the enumeration failure found on 2026-09-25.
 *
 * The value mirrors the lab's generated RK3568 kernel header
 * (work/netbsd-rk3568/obj/.../compile/RK3568_NFS/usb_dma.h:
 * `#define NUSB_DMA 1`): files.usb defines one `usb_dma` attachment per
 * `usb` (HCD), and this image drives one HCD.  The upstream file's
 * _LOCORE/_KERNEL_OPT_* asm block is a linker-time assertion for the
 * objcopy'd assembly path and has no meaning here.
 */

#ifndef _USB_DMA_H_
#define _USB_DMA_H_

#define	NUSB_DMA	1

#endif /* _USB_DMA_H_ */