/*
 * @file   dbg_scenario.h
 * @brief  The UP carrier's scenario runner (D56).
 *
 * The ThreadX UP image has no shell: this task is its only "operator".
 * Every scenario run is gated by a gdb breakpoint - the stub owns the
 * target between runs, which is what makes the carrier deterministic.
 */

#ifndef FREEWEBCAMERA_DBG_SCENARIO_H
#define FREEWEBCAMERA_DBG_SCENARIO_H

/* Scenario selector, read after every stub break. The host picks the next
 * run without any console input path: `set var dbg_scenario=N`, then
 * continue. Negative or out-of-range values are reported and reset to 0. */
extern volatile int dbg_scenario;

/* Scenario 7 (wl-load) knobs and status, all host-settable/readable from
 * the stub. Defaults are the standard wedge experiment: 30s TCP uplink
 * against 192.168.0.18, auto-park after 5s of frozen lwIP progress. */
extern char dbg_wl_ssid[32];
extern char dbg_wl_psk[64];
extern volatile unsigned long dbg_wl_host;	  /* server, host order */
extern volatile unsigned long dbg_wl_secs;	  /* iperf duration */
extern volatile unsigned long dbg_wl_stall_secs;  /* park threshold */
extern volatile unsigned long dbg_wl_timeout_secs; /* assoc+DHCP budget */
extern volatile unsigned long dbg_wl_max_parks;
extern volatile int dbg_wl_reverse;		  /* 1 = downlink (-R) */
extern volatile int dbg_wl_state; /* 1 enum 2 connect 3 wait-ip 4 load 5 park 6 done 7 fail */
extern volatile unsigned long dbg_wl_ip;
extern volatile unsigned long dbg_wl_xmit;
extern volatile unsigned long dbg_wl_recv;
extern volatile unsigned long dbg_wl_stalled;
extern volatile unsigned long dbg_wl_parks;
extern volatile unsigned long dbg_wl_voq_info;
extern volatile unsigned long dbg_wl_txdma_status;

/* The carrier's one thread (created by board_main under THREADX_UP_BUILD).
 * Breaks into the stub immediately on entry, then loop: break - run
 * table[dbg_scenario] - break. */
void dbg_scenario_task(void *argument);

#endif /* FREEWEBCAMERA_DBG_SCENARIO_H */
