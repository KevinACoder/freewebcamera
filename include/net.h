/*
 * @file   net.h
 * @brief  Interface-layer seam for the network stack.
 *
 * app/ (and anything above the adapters) calls net_start() and nothing else;
 * which stack runs, how many netifs exist and how they are fed live behind
 * this header in the adapter that implements it (the shell.h precedent).
 * Currently that is lwIP on the ThreadX/CMSIS twin, with the wlan netif
 * bridge from the net80211 adapter.
 */

#ifndef FREEWEBCAMERA_NET_H
#define FREEWEBCAMERA_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring the network stack up: start the stack's own thread(s) and register
 * the netifs. Call once, from a thread, after the kernel is running. Returns
 * 0 when the stack start was initiated; a failure means no netif will ever
 * carry traffic, which the caller reports and the system otherwise ignores. */
int net_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_NET_H */
