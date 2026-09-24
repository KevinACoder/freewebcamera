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

/* Register the lwIP presentation (rx hooks + wlan netif). Lazy: the
 * TCP/IP thread must be up, so this cannot run inside wlan_start();
 * the shell commands and the supplicant glue call it on first use.
 * Returns 0 once registered. */
int wlan_lwip_start(void);

/* Console output goes live only after the USART driver is
 * initialized (wlan_console.c). */
void wlan_console_ready(void);

#endif /* FREEWEBCAMERA_WLAN_ADAPTER_H */
