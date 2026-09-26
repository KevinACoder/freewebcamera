/* config(8)-generated stand-in for the rtw8189f(4) tunables (upstream's
 * files.sdmmc defflags it with RTW8189F_DEBUG only, which stays off).
 *
 * The one define that matters here is the worker's RX poll quantum:
 * upstream sleeps mstohz(50) (its ~47 ms ping RTT floor comes from that);
 * this port runs hz=100, so the wait granularity is one 10 ms tick - and
 * the tighter tick also cuts the TX-done reclaim latency (the frozen
 * workspace's KI-040 territory, carried from its M11 tuned image). */

#ifndef _OPT_RTW8189F_H_
#define _OPT_RTW8189F_H_

#ifndef RTW8189F_RX_POLL_MS
#define RTW8189F_RX_POLL_MS	10
#endif

#endif /* _OPT_RTW8189F_H_ */
