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
 *  - The thread priorities below are FreeRTOS priorities, not CMSIS-RTOS2
 *    ones: lwIP's own sys_thread_new (vendored verbatim in
 *    third-party/lwip/contrib/ports/freertos) calls xTaskCreate directly, so
 *    these numbers go straight into the scheduler. The adapter's own threads
 *    are created through CMSIS-RTOS2 instead, whose mapping divides the CMSIS
 *    band number by 8 (cmsis_rtos2/cmsis_os2_impl.c), i.e.
 *    osPriorityLow=1, osPriorityNormal=3, osPriorityAboveNormal=4,
 *    osPriorityHigh=5. Keep the two sets consistent: tcpip outranks the
 *    receive threads it is fed by, and the console shell outranks tcpip.
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
#define MEM_SIZE                        (64 * 1024)
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
#define MEMP_NUM_TCPIP_MSG_API          16
#define MEMP_NUM_TCPIP_MSG_INPKT        16
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
 * does not fit - no driver counter sees it. The rtw8189f worker
 * drains every 10 ms, so steady rates survive (UDP 2.6 Mbit/s
 * passes), but a TCP burst up to the advertised window lands as one
 * over-the-air burst: at 32*MSS the window (46 KB) exceeded the FIFO
 * and the data connection lost 15 of its first 17 segments (M11
 * round 2, board + pktmon evidence); 8*MSS was marginal - stall after
 * seconds-to-minutes when a burst coincided with ambient broadcast
 * junk. 4*MSS keeps a full-window burst well inside the FIFO;
 * ceiling ~4 Mbit/s at the 11 ms air RTT, above the measured rates.
 * Partial revert of 9a9df36's RCV side (that fix targets the USB
 * lane, which has no such FIFO); D52 in DESIGN §14. */
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_QUEUE_OOSEQ                 0
#define LWIP_WND_SCALE                  0
#define LWIP_TCP_SACK_OUT               0

/* --- threads and mailboxes ------------------------------------------------ */

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          4096
/* FreeRTOS priority 4 == osPriorityAboveNormal: above the receive threads,
 * below the console shell (5) and the timer task (7). */
#define TCPIP_THREAD_PRIO               4
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_THREAD_STACKSIZE        2048
#define DEFAULT_THREAD_PRIO             3
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
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