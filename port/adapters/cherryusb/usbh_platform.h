/*
 * @file   usbh_platform.h
 * @brief  The CherryUSB backend's platform entry: bring the RK3568 USB
 *         domain and both EHCI roots up, with the hub threads running.
 *
 * Idempotent; task context only.
 *
 * @date   27.09.2026
 * @author zhugengyu
 */

#ifndef USBH_PLATFORM_H
#define USBH_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

/* OSAL init (byte pool + reaper), the shared usb2phy1 domain, then
 * register + initialize one CherryUSB bus per EHCI root, wait for each
 * controller to come live, and manufacture the connect edge for devices
 * that were already plugged in at boot (KI-006). Returns 0 when every
 * compiled-in root is up. */
int usbh_platform_start(void);

/* True once usbh_platform_start() has completed successfully. */
bool usbh_platform_ready(void);

#endif /* USBH_PLATFORM_H */
