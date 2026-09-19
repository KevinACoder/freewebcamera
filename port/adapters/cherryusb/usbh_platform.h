/*
 * @file   usbh_platform.h
 * @brief  The RK3568 USB platform bring-up the EHCI glue calls before the
 *         controller registers are touched.
 *
 * Idempotent: the first call runs the full sequence, later calls return
 * immediately (both EHCI buses share one usb2phy1 domain).
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#ifndef USBH_PLATFORM_H
#define USBH_PLATFORM_H

/* Full usb2phy1 domain for the two panel EHCI roots (runs the shared bus
 * domain first). The xHCI line does NOT use this: its PHY/platform sequence
 * belongs to U-Boot preboot `usb start` (decision D36). */
void usbh_rk3568_usb2phy1_domain_init(void);

#endif /* USBH_PLATFORM_H */
