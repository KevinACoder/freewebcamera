/*
 * @file   usbh_platform.h
 * @brief  The RK3568 USB platform bring-up the host-controller glue calls
 *         before the controller registers are touched.
 *
 * Idempotent: the first call runs the full sequence, later calls return
 * immediately (both EHCI buses share one usb2phy1 domain; the xHCI image
 * owns the USB3OTG domain).
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#ifndef USBH_PLATFORM_H
#define USBH_PLATFORM_H

/* Full usb2phy1 domain for the two panel EHCI roots (runs the shared bus
 * domain first). */
void usbh_rk3568_usb2phy1_domain_init(void);

/* Full USB3OTG domain for the DWC3/xHCI USB3 socket group: bus domain,
 * USB3OTG clock gates, SRST_USB3OTG0/1 + USB2HOST pulse, usb2phy0 port GRF
 * and the dwc3 core reconfig (soft reset, PHY quirks, PRTCAP=host) - the
 * board-proven NetBSD rk_usb2phy.c + dwc3_fdt.c sequence (D38). */
void usbh_rk3568_usb3otg_domain_init(void);

#endif /* USBH_PLATFORM_H */
