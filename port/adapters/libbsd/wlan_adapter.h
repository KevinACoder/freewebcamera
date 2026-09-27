/*
 * @file
 * @brief Adapter-internal declarations shared by the net80211
 * adapter's translation units.
 *
 * @author zhugengyu
 * @date 22.09.2026
 */

#ifndef FREEWEBCAMERA_WLAN_ADAPTER_H
#define FREEWEBCAMERA_WLAN_ADAPTER_H

/* (The lwIP presentation used to be registered lazily from here via
 * wlan_lwip_start(); since the lwip feat it boots from the lwip adapter
 * instead - net_start() runs wlan_lwip_init() at boot, "net: READY".) */

/* Console output goes live only after the USART driver is
 * initialized (wlan_console.c). */
void wlan_console_ready(void);

#endif /* FREEWEBCAMERA_WLAN_ADAPTER_H */
