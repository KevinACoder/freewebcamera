/*
 * @file   wlan.h
 * @brief  WLAN subsystem interface.
 *
 * The application-facing surface of the 802.11 lane: start it. The
 * stack itself (NetBSD net80211 + the urtwn USB driver, vendored under
 * third-party/net80211/) is bound to the image by the net80211
 * adapter; this header exists so app/ never sees the library's own
 * port headers, same rule as net.h.
 *
 * The wireless controls (scan, join, supplicant) stay shell-side: they
 * are operator commands, not boot-time state, and freezing them into
 * the interface now would out-run the M7 decisions they belong to.
 *
 * @author zhugengyu
 * @date   22.09.2026
 */

#ifndef FREEWEBCAMERA_WLAN_H
#define FREEWEBCAMERA_WLAN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the wlan subsystem: the library's OS services (locks,
 * sleeps, timers), the firmware registry and the lwIP presentation
 * once the TCP/IP thread is up.
 *
 * MUST run before usb_start(): the USB class hook claims a matched
 * adapter during enumeration and drives the driver attach on the hub
 * thread, which needs those services to exist. It does not touch the
 * radio - an attached adapter powers up through the shell (`wlan up`)
 * or the supplicant (`wpa start`).
 *
 * Context: thread only. Idempotent: the second call returns 0 without
 * doing anything. Returns 0 when the services are in place.
 */
int wlan_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_WLAN_H */
