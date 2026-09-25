/*
 * @file   lwipopts.h
 * @brief  lwIP configuration for the RK3568 carrier.
 *
 * This file is the whole of the stack's configuration: lwIP reaches it from
 * lwip/opt.h in every translation unit. The choices worth explaining:
 *
 *  - NO_SYS=0: there is a kernel, so lwIP runs its own thread (the tcpip
 *    thread) and the wlan bridge hands it frames through tcpip_input. The
 *    alternative (NO_SYS=1, a polled loop) would pull the whole net80211
 *    delivery path into one context, which is not where this project is
 *    heading.
 *
 *  - MEM_LIBC_MALLOC=0: lwIP keeps its own static heap (MEM_SIZE) and its own
 *    static pools rather than calling malloc(). Two reasons: this image is
 *    freestanding (-nostdlib, no libc malloc at all) and a static pool makes
 *    the stack's memory use visible at link time instead of showing up as
 *    runtime heap exhaustion.
 *
 *  - IPv6, autoip/ACD, PPP and the app directories are off. The sequential
 *    API (netconn + sockets) lands with the netutils line: iperf3_embedded
 *    and the ported netutils tools (ping/tftp/ntp/telnet/netio/tcpdump) are
 *    BSD-socket citizens, so LWIP_NETCONN/LWIP_SOCKET/LWIP_RAW/LWIP_DNS turn
 *    on together with their api/ and dns.c files (see the Makefile's
 *    LWIP_SRCS). This line ends at DHCP + ICMP/UDP/TCP over the wlan netif.
 *
 *  - Thread priorities here are CMSIS BANDS (sys_arch.c maps band*8 onto
 *    osPriority): the ladder this image relies on is
 *      shell(5) > tcpip(4) >= supplicant(4) > usbdi worker(<=3) > idle.
 *    Two lessons of the old workspace are baked into the numbers:
 *      D54 - a driver-context thread above tcpip stalls INPKT delivery and
 *            silently drops the mailbox; keep the usbdi worker under band 4.
 *      D54 - the supplicant must outrank the usbdi worker, or the worker's
 *            scan polling starves the eloop thread and scans never complete.
 *
 * @author zhugengyu
 * @date   25.09.2026
 */

#ifndef FREEWEBCAMERA_LWIPOPTS_H
#define FREEWEBCAMERA_LWIPOPTS_H

/* --- OS integration ------------------------------------------------------- */

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         0
/* The sequential API (netconn/sockets) is the netutils line's entire surface:
 * iperf3_embedded and the ported netutils tools are socket citizens. Netconn
 * is the layer sockets.c sits on, so the two come back as a pair and their
 * api/ files join LWIP_SRCS in the same commit. */
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
/* iperf3_embedded sets SO_RCVTIMEO; netutils' tftp/ntp/ping rely on it too. */
#define LWIP_SO_RCVTIMEO                1
#define LWIP_NETIF_API                  1
#define LWIP_TIMERS                     1

/* --- memory --------------------------------------------------------------- */

#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
/* The bridge stages a received frame into a PBUF_RAM from this heap while
 * still in the usbdi worker; 64K made RX bursts fail the allocation under
 * concurrent traffic. */
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
#define LWIP_ETHERNET                   1
#define LWIP_ARP                        1
#define LWIP_ICMP                       1
/* igmp.c / autoip.c / acd.c are not compiled: switching these on would mean
 * adding the file to LWIP_SRCS at the same time. dns.c IS compiled (netutils
 * line: ping/ntp resolve names). */
#define LWIP_IGMP                       0
#define LWIP_DNS                        1
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0
#define LWIP_UDP                        1
#define LWIP_TCP                        1
/* The raw API is back with the netutils ping (SOCK_RAW/ICMP): raw.c was
 * already in the compile set, this switch arms it. */
#define LWIP_RAW                        1
/* struct timeval comes from the toolchain's <sys/time.h> (the netutils
 * tools include it directly); lwIP must not define its own private copy or
 * the two clash. arch/cc.h pulls the header in for every lwIP TU. */
#define LWIP_TIMEVAL_PRIVATE            0
/* No libc behind this image: lwIP supplies its own errno constants
 * (lwip/errno.h) instead of reaching for <errno.h>, whose newlib shape
 * needs the reent machinery. err.c's err-to-errno table (compiled whenever
 * NO_SYS=0) then finds its constants, and the errno variable itself is
 * defined once in the adapter (lwip_diag.c). */
#define LWIP_PROVIDE_ERRNO              1

/* --- pool sizes ----------------------------------------------------------- */

#define MEMP_NUM_TCP_PCB                16
#define MEMP_NUM_TCP_PCB_LISTEN         4
/* >= TCP_SND_QUEUELEN, per lwIP's own sizing rule. */
#define MEMP_NUM_TCP_SEG                128
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_RAW_PCB                8
#define MEMP_NUM_NETBUF                 4
/* One netconn per socket user, worst case all running at once: iperf3 (1),
 * telnet server + one session (2), tftp client/server (1), ntp (1),
 * netio server (1), spare (4). */
#define MEMP_NUM_NETCONN                10
#define MEMP_NUM_TCPIP_MSG_API          16
/* D54: 16 dropped 1799 messages over a 600s downlink once the driver worker
 * outranked tcpip; 64 is the number the old workspace converged on. */
#define MEMP_NUM_TCPIP_MSG_INPKT        64
#define MEMP_NUM_SYS_TIMEOUT            16
#define MEMP_NUM_FRAG_PBUF              16

/* --- TCP ------------------------------------------------------------------ */

#define TCP_MSS                         1460
/* D52: a full 32*MSS window bursts past the RTL8188F-family 16K RX FIFO and
 * ends in silent frame loss over the air; 8*MSS is the wlan-line value. The
 * sustained-throughput line revisits this with iperf3, one variable at a
 * time. */
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (32 * TCP_MSS)
/* Must be >= 4 * TCP_SND_BUF / TCP_MSS: 4 * 46720 / 1460 = 128. */
#define TCP_SND_QUEUELEN                128
#define TCP_QUEUE_OOSEQ                 0
#define LWIP_WND_SCALE                  0
#define LWIP_TCP_SACK_OUT               0

/* --- threads and mailboxes ------------------------------------------------ */

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          4096
/* Band 4 (osPriorityAboveNormal): above its feeders, below the shell. */
#define TCPIP_THREAD_PRIO               4
#define TCPIP_MBOX_SIZE                 64
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
 * (DESIGN G18). Enable per-file with LWIP_DBG_ON while debugging, not here. */
#define LWIP_DEBUG                      0
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

#endif /* FREEWEBCAMERA_LWIPOPTS_H */
