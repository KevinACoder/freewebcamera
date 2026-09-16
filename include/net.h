/*
 * @file   net.h
 * @brief  Network stack interface.
 *
 * The one thing the application layer needs from the network: start it. What
 * "it" is - lwIP over the two GMAC ports today - belongs to the adapter
 * (port/adapters/lwIP/), and swapping the stack changes that directory and
 * this function's implementation, not its callers.
 *
 * Nothing about addresses, ports or link state appears here on purpose. The
 * application configures those with time (the shell's `net` command), and a
 * larger set of hooks now would freeze decisions that are still moving.
 *
 * @author zhugengyu
 * @date   16.09.2026
 */

#ifndef FREEWEBCAMERA_NET_H
#define FREEWEBCAMERA_NET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the network: initialize the registered Ethernet MAC and PHY
 * instances, register their interfaces with the stack, and start the stack's
 * threads and the link monitor.
 *
 * Returns 0 when the stack is running. A port whose PHY never answers costs
 * the rest of the bring-up nothing in this signature: the stack comes up, that
 * port simply has no link, and the `net` command says so.
 *
 * Context: must be called from a thread, with the scheduler running. It
 * creates threads, waits for autonegotiation (tens to hundreds of
 * milliseconds) and, on the driver side, delays - none of which can happen
 * from the boot path.
 *
 * Idempotent: the second call returns 0 without doing anything.
 */
int net_start(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_NET_H */