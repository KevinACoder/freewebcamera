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
 * USB3OTG clock gates, SRST_USB3OTG0/1 + USB2HOST pulse and the usb2phy0
 * port GRF run once; the dwc3 core reconfig (soft reset, PHY quirks,
 * PRTCAP=host) runs once per instance - the board-proven NetBSD
 * rk_usb2phy.c + dwc3_fdt.c sequence (D38). instance 0 = 0xFD000000,
 * instance 1 = 0xFCC00000 (otg-as-host). */
void usbh_rk3568_usb3otg_domain_init(uint8_t instance);

#endif /* USBH_PLATFORM_H */
