/*
 * @file   lwipopts.h
 * @brief  lwIP configuration for the RK3568 carrier.
 *
 * This file is the whole of the stack's configuration: lwIP reaches it from
 * lwip/opt.h in every translation unit. The choices worth explaining:
 *
 *  - NO_SYS=0: there is a kernel, so lwIP runs its own threads (tcpip thread,
 *    per-netif receive threads) and offers the sequential API. The alternative
 *    (NO_SYS=1, a polled loop) cannot host an HTTP server without hand-rolling
 *    the socket layer, which is where this project is heading.
 *
 *  - MEM_LIBC_MALLOC=0: lwIP keeps its own static heap (MEM_SIZE) and its own
 *    static pools rather than calling malloc(). Two reasons: this image is
 *    freestanding (-nostdlib, no libc malloc at all) and a static pool makes
 *    the stack's memory use visible at link time instead of showing up as
 *    FreeRTOS heap exhaustion.
 *
 *  - IPv6, sockets, DNS, autoip/ACD, PPP and the app directories are off, and
 *    the files for them are not even vendored (see the Makefile's LWIP_SRCS).
 *    The httpd lands in the next milestone and only needs the raw API.
 *
 *  - Thread priorities are kernel-flavored and the two image lines differ
 *    (M11 r4 audit). The ThreadX line's lwIP sys_arch
 *    (port/adapters/lwip/cmsis/sys_arch.c) maps TCPIP_THREAD_PRIO through
 *    the CMSIS bands: 4 (osPriorityAboveNormal) becomes raw ThreadX
 *    priority 15, where smaller means MORE urgent. The FreeRTOS line's
 *    vendored sys_arch passes the same 4 straight to xTaskCreate. Either
 *    way tcpip must outrank the wlan workers feeding it - wlan_adapter.c
 *    resolves WLAN_WORK_PRIORITY per kernel (16 raw on ThreadX, 5 before
 *    the FreeRTOS inversion) for exactly that. The console shell sits at
 *    osPriorityNormal = ThreadX raw 19, so on the ThreadX line tcpip
 *    outranks the shell and console interaction can lag under a bulk
 *    flow; on FreeRTOS the shell (5) outranks tcpip (4).
 */

#ifndef FREEWEBCAMERA_LWIPOPTS_H
#define FREEWEBCAMERA_LWIPOPTS_H

/* --- OS integration ------------------------------------------------------- */

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
/* tcpip thread + mailboxes: the model the vendored FreeRTOS port implements. */
#define LWIP_TCPIP_CORE_LOCKING         0
#define LWIP_NETCONN                    1
/* On for the iperf3 throughput client (third-party/iperf3_embedded); its
 * socket code rides the netconn API that is already compiled. */
#define LWIP_SOCKET                     1
#define LWIP_SO_RCVTIMEO                1
/* netifapi_*: the shell's `net` commands change addresses from another thread. */
#define LWIP_NETIF_API                  1
#define LWIP_NETCONN_FULLDUPLEX         0
#define LWIP_NETCONN_SEM_PER_THREAD     0
#define LWIP_TIMERS                     1

/* --- memory --------------------------------------------------------------- */

#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
/* 128 KB: the net80211 bridge allocates every RX frame as
 * pbuf_alloc(PBUF_RAM) + pbuf_take from this heap (three-copy SDIO path),
 * so heap churn scales with the frame rate; at the 12 Mbit/s class rates
 * this round targets that is ~1000 alloc/free of 1.5 KB per second.
 * 64 KB held at the 1-2 Mbit/s rates, 128 KB keeps headroom without
 * squeezing the wlan/USB pools (M11 r4). */
#define MEM_SIZE                        (128 * 1024)
/* 8, not lwIP's 4-byte default: aarch64 pointers are 8 bytes and mem_malloc
 * must hand back 8-aligned blocks or every pbuf's pointer fields are asked to
 * be read unaligned. */
#define MEM_ALIGNMENT                   8
#define MEMP_OVERFLOW_CHECK             0
#define MEMP_SANITY_CHECK               0
#define PBUF_POOL_SIZE                  40
/* Large enough for a 1518-byte frame plus link header, with headroom: a
 * jumbo-ish single-pbuf receive must never need a chain. */
#define PBUF_POOL_BUFSIZE               1600
#define LWIP_NETIF_TX_SINGLE_PBUF       1

/* --- protocols ------------------------------------------------------------ */

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ICMP                       1
/* igmp.c / dns.c / autoip.c are not vendored: switching these on would need
 * adding the file to LWIP_SRCS at the same time. */
#define LWIP_IGMP                       0
#define LWIP_DNS                        0
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_RAW                        1
/* No libc behind this image: lwIP supplies its own errno constants
 * (lwip/errno.h) instead of reaching for <errno.h>, whose newlib shape
 * needs the reent machinery. With sockets on, sockets.c now also reads
 * the errno variable - defined once in the adapter (lwip_diag.c). */
#define LWIP_PROVIDE_ERRNO              1

/* --- pool sizes ----------------------------------------------------------- */

#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_PCB_LISTEN         4
/* >= TCP_SND_QUEUELEN, per lwIP's own sizing rule. */
#define MEMP_NUM_TCP_SEG                128
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_RAW_PCB                8
#define MEMP_NUM_NETBUF                 32
#define MEMP_NUM_NETCONN                32
/* Mailbox pressure: TCPIP_MBOX_SIZE below must stay <= the INPKT pool
 * (every inbound post consumes one MEMP_TCPIP_MSG_INPKT), and the API
 * pool covers the netconn-driven share of the same mailbox (bounded by
 * MEMP_NUM_NETCONN). The 16-deep configuration was exhausted 1799 times
 * in one 600 s TCP downlink (M11 r4 baseline); 64/64/32 lets the tcpip
 * thread absorb an RX burst without dropping segments into retransmit. */
#define MEMP_NUM_TCPIP_MSG_API          32
#define MEMP_NUM_TCPIP_MSG_INPKT        64
/* PBUF_ROM/PBUF_REF pbufs come from this pool (PBUF_RAM takes the heap
 * above); no current path leans on it - pinned explicitly instead of
 * riding lwIP's default 16. */
#define MEMP_NUM_PBUF                   32
#define MEMP_NUM_SYS_TIMEOUT            16
#define MEMP_NUM_FRAG_PBUF              16

/* --- TCP ------------------------------------------------------------------ */

#define TCP_MSS                         1460
/* Send window: 8*MSS x ~40ms air RTT capped iperf uplink at ~2 Mbit/s
 * (BDP); 32*MSS (the M7P bump) lets the same link reach its air-rate
 * ceiling. Kept - the TX side has no hardware FIFO constraint. */
#define TCP_SND_BUF                     (32 * TCP_MSS)
/* Must be >= 4 * TCP_SND_BUF / TCP_MSS: 4 * 46720 / 1460 = 128. */
#define TCP_SND_QUEUELEN                128
/* Receive window vs the RTL8188F's RX FIFO: the SDIO chip buffers
 * received frames in a 16 KB RX FIFO (rtl8189fs RX_DMA_SIZE_8188F =
 * 0x4000, 128/256 B reserved for C2H/txrpt) and silently drops what
 * does not fit - no driver counter sees it. History: the rtw8189f
 * worker drained every 10 ms, so a full-window burst sat in the FIFO
 * for up to a poll quantum - at 32*MSS the window (46 KB) exceeded
 * the FIFO and the data connection lost 15 of its first 17 segments
 * (M11 round 2); 8*MSS was marginal; 4*MSS was the safe pick (D52).
 * M11 r4 B2: the DAT1 card interrupt drains within microseconds of
 * the first frame, and the FIFO fills at air rate (~20 Mbit/s class)
 * while SDIO empties it at 200 Mbit/s - occupancy now stays far
 * below one window. 8*MSS (11.7 KB) is the measured-RTT BDP step
 * (avg 13 ms, ~7 Mbit/s); window growth beyond this needs the
 * PBUF_POOL/heap behind it, not the FIFO. */
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_QUEUE_OOSEQ                 0
#define LWIP_WND_SCALE                  0
#define LWIP_TCP_SACK_OUT               0

/* --- threads and mailboxes ------------------------------------------------ */

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          4096
/* FreeRTOS: straight xTaskCreate priority 4. ThreadX: CMSIS band 4
 * (osPriorityAboveNormal) -> raw ThreadX priority 15, small = urgent.
 * On both lines it must sit ABOVE the wlan workers (WLAN_WORK_PRIORITY
 * in wlan_adapter.c resolves the matching pair) so the stack drains
 * its mailboxes faster than the drivers fill them. */
#define TCPIP_THREAD_PRIO               4
/* 64 == MEMP_NUM_TCPIP_MSG_INPKT: the mbox and its message pool exhaust
 * together, so only their common depth matters. */
#define TCPIP_MBOX_SIZE                 64
#define DEFAULT_THREAD_STACKSIZE        2048
#define DEFAULT_THREAD_PRIO             3
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
/* Socket receive path: tcpip hands delivered segments to the netconn's
 * own mailbox before the application recv()s them; 8 slots backed up
 * within milliseconds at bulk rates (iperf3_embedded recv loops at
 * 4096 B). 32 keeps a full TCP window's worth of segments queued. */
#define DEFAULT_TCP_RECVMBOX_SIZE       32
#define DEFAULT_ACCEPTMBOX_SIZE         4

/* --- netif ---------------------------------------------------------------- */

#define LWIP_SINGLE_NETIF               0
#define LWIP_HAVE_LOOPIF                0
#define LWIP_NETIF_LOOPBACK             0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_HOSTNAME             0
#define LWIP_NETIF_EXT_STATUS_CALLBACK  0
#define LWIP_CHECKSUM_ON_COPY           0

/* Platform facts (byte order, rand, diag hooks, which libc headers exist) live
 * in arch/cc.h, which is where lwIP looks for them. */

/* --- debugging ------------------------------------------------------------ */

/* LWIP_DEBUG stays off: every diag call would go out the polled console at
 * 115200 baud, which the lab has already seen drag the network path to a halt
 * (DESIGN G18). Enable per-file with LWIP_DBG_ON while debugging, not here.
 *
 * Stats stay on permanently: the counters cost a few hundred bytes of
 * static RAM, print nothing on their own, and the shell's `lwstats` dump
 * is the only window into the tcpip-thread state (drops, memerrs, mbox
 * pressure) when a bulk flow wedges - the M11 TCP-downlink chase had to
 * run blind without it. */
#define LWIP_DEBUG                      0
#define LWIP_STATS                      1
#define LWIP_STATS_DISPLAY              1

#endif /* FREEWEBCAMERA_LWIPOPTS_H */