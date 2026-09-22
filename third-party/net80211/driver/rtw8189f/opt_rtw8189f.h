/*
 * @file
 * @brief kernel option: rtw8189f tunables and debug defaults.
 */

#ifndef _OPT_RTW8189F_H_
#define _OPT_RTW8189F_H_

/* #define RTW8189F_DEBUG - off */

/* PORT: the worker's RX poll quantum.  NetBSD sleeps mstohz(50) (its
 * ~47 ms ping RTT floor comes from this); the RTOS ports run hz=100,
 * so the wait granularity is one 10 ms tick - and the tighter tick
 * also cuts the TX-done reclaim latency (KI-040 territory). */
#ifndef RTW8189F_RX_POLL_MS
#define RTW8189F_RX_POLL_MS	10
#endif

#endif /* _OPT_RTW8189F_H_ */
