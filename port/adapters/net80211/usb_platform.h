/*
 * @file
 * @brief The usb platform bring-up entry (wlan_adapter calls it after
 * the osal is up).
 */

#ifndef NET80211_USB_PLATFORM_H_
#define NET80211_USB_PLATFORM_H_

#include <stdint.h>

/* Bring up the usb2phy1 domain + the panel EHCI root and start the
 * NetBSD usbus chain on it. Returns 0 on success. */
int usb_platform_init(void);
void usb_platform_dump(void);
void usb_platform_reg_dump(void);
void usb_platform_qh_dump(void);

/* the usb history ring: every state transition the imported core
 * logged, oldest first (max = 0 prints the whole ring) */
void usb_platform_hist_dump(unsigned int max);
/* requested against measured wait times for delay()/usb_delay_ms() */
void usb_platform_delay_test(void);

#endif /* NET80211_USB_PLATFORM_H_ */
