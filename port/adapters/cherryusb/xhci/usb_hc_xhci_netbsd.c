/*
 * @file   usb_hc_xhci_netbsd.c
 * @brief  DWC3 xHCI host controller driver (CherryUSB usb_hc.h contract) for
 *         the rk3568 usbhost_dwc3 @ 0xFD000000 (xHCI regs at +0).
 *
 * Derived from NetBSD netbsd-11 sys/dev/usb/xhci.c (rev 1.188.2.4) /
 * xhcireg.h (rev 1.25), BSD-2-Clause, (c) The NetBSD Foundation and
 * contributors - the license notice follows this block. That driver is the
 * one xHCI implementation board-proven on this SoC from a cold USB domain
 * (2026-09-03: both USB3 Type-A sockets enumerating, HS bulk saturating a
 * USB stick; re-verified at every NetBSD cold boot since). It replaces the
 * standalone-line transplant of 2026-09-19/20, which - in the inherited
 * U-Boot environment - never got past Address Device (Parameter Error 17,
 * decision D37); the NetBSD platform+xHCI stack is the only reference that
 * covers the whole path end to end (decision D38).
 *
 * Fidelity rules: every register write, field value and ordering constraint
 * follows NetBSD xhci.c - hc_reset (CNR -> halt -> HCRST self-clear -> CNR),
 * PAGESIZE-derived page size, scratchpad via DCBAA[0], CONFIG MaxSlotsEn
 * read-modify-write, IMAN=ENA/IMOD=0, single-step Address Device, ERDP
 * updates carrying EHB, the atomic first-TRB cycle flip when enqueuing a
 * batch. Mechanical deviations are limited to the contract shell:
 *   - CherryUSB calls (usbh_submit_urb/roothub/IRQ) instead of usbdi;
 *   - synchronous poll-wait completion (hub-thread context) instead of
 *     condvars, with the NetBSD command-abort path on timeout;
 *   - Address Device runs BSR=1 first (device reachable at addr 0) because
 *     CherryUSB reads the first 8 descriptor bytes before SET_ADDRESS -
 *     NetBSD addresses in one BSR=0 step at usbd_new_device time;
 *   - a post-RUN port power pass (the vendored hub class never issues
 *     SetFeature(PORT_POWER) to a roothub);
 *   - root-hub ClearFeature writes only PP | target W1C bit (PORTSC is
 *     W1C/RW mixed; writing back a stale PED snapshot disables the port -
 *     measured 2026-09-20).
 *
 * HS/FS (USB2 only - the SS lane's combphy serves SATA). Cache discipline:
 * rings/contexts flushed after build, event ring + output contexts
 * invalidated before CPU reads, via the glue's usb_dcache_*.
 *
 * @author zhugengyu
 * @date   20.09.2026
 */
/*	$NetBSD: xhci.c,v 1.188.2.4 $	*/
/*-
 * Copyright (c) 2013 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Christos Zoulas and Nick Hudson.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "usbh_core.h"
#include "usb_hc_xhci.h"
#include "usb_hub.h"
#include "usbh_hub.h"

#include <string.h>

/* Glue-provided platform hooks (usb_hc.h carries no prototypes for these;
 * the EHCI driver declares them weak in its own file). */
extern void usb_hc_low_level_init(struct usbh_bus *bus);
extern void usb_hc_low_level_deinit(struct usbh_bus *bus);
extern void *usb_sys_malloc_align(size_t align, size_t size);
extern unsigned long usb_hc_get_register_base(uint32_t id);
extern void usb_hc_enable_interrupt(uint32_t id);
extern void usb_hc_disable_interrupt(uint32_t id);

/* ===================== 常量 ===================== */
/* NetBSD: XHCI_MAX_DCI 31, 命令/事件环 256 TRB(TRB 数经 usb_config.h 可调),
 * 传输环 256 TRB。DCI 编码即 xHCI spec §4.5.2: ep0=1, OUT n=2n, IN n=2n+1。 */
#define XHCI_MAX_DCI         31U
#define XHCI_MAX_SLOTS       64U
#define XHCI_EVENT_RING_SIZE CONFIG_USBHOST_XHCI_EVENT_RING_SIZE
#define XHCI_CMD_RING_SIZE   CONFIG_USBHOST_XHCI_COMMAND_RING_SIZE
#define XHCI_TRANSFER_RING_SIZE CONFIG_USBHOST_XHCI_TRANSFER_RING_SIZE

#define TRB_MAX_BUFF_SIZE    0x10000U /* 单 TRB 缓冲不得跨 64KB 边界 */

#define USB2_DIR_IN          0x80U
#define USB2_EP_NUM_MASK     0x0FU

#define CMD_TIMEOUT_MS       5000U /* NetBSD USBD_DEFAULT_TIMEOUT */
#define CTRL_TIMEOUT_MS      5000U

/* 本 DWC3 寄存器窗只支持 32 位访问: CAPLENGTH/HCIVERSION 这类子字寄存器
 * 用 32 位读 + 移位取字段(NetBSD XHCI_32BIT_ACCESS 的 RMW 语义)。 */
#define XHCI_HCIVERSION_0_96 0x0096U
#define XHCI_HCIVERSION_1_10 0x0110U

/* ===================== 内部结构 ===================== */

/* TD 簿记: 每个 TRB 槽都存 td 指针, 事件无论落在 TD 内哪个 TRB 都能找到。
 * kind/expect_status 服务控制传输的短包语义: data 段短包(ISP)先来一个事件
 * (记 residual), status 段事件才是终态; 其余情况事件即终态(NetBSD 单
 * NORMAL TRB 的 bulk/intr 与本实现的分段 control 同此)。 */
struct xhci_td {
    struct usbh_urb *urb;
    volatile uint32_t state;   /* 0=pending 1=done ok 2=done err */
    uint32_t actual;
    int error;
    uint8_t kind;              /* 0=normal(bulk/intr) 1=control */
    uint8_t first_idx;
    uint8_t last_idx;
    uint8_t short_seen;
};

struct xhci_ring {
    struct xhci_trb *trbs;
    struct xhci_td *tds;       /* [num_trbs], 每个 TRB 槽一条 */
    uint32_t num_trbs;
    uint32_t ep;               /* enqueue index (NetBSD xr_ep) */
    uint8_t cs;                /* consumer/converter cycle (NetBSD xr_cs) */
    uint32_t dequeue;          /* 空间核算用(完成处推进) */
};

/* 上下文寻址: CSZ(HCCPARAMS1 bit2)决定每上下文 64/32 字节(NetBSD sc_ctxsz)。
 * 输入: 控制 ctx 在槽 0(u-boot 风格占满一个 ctxsz), ctx(dci) 在 ctxsz*(dci+1);
 * 输出(DCBAA->dev_ctx): slot ctx 在 0, ctx(dci) 在 ctxsz*dci。 */
struct xhci_dev {
    uint8_t in_use;
    uint8_t slot_id;
    struct usbh_hubport *hport;
    uint16_t ep0_mps_hw;       /* 硬件当前 ep0 MPS */
    uint32_t ep_configured;    /* 已 Configure Endpoint 的 dci 位图 */
    struct xhci_ring *ep_rings[XHCI_MAX_DCI];
    uint8_t *input_ctx;        /* 64B 对齐, (2+DCI_MAX) 个 ctx 槽 */
    uint8_t *dev_ctx;          /* 64B 对齐, (1+DCI_MAX) 个 ctx 槽 */
    uint32_t ctxsz;
};

struct xhci_hcd {
    struct usbh_bus *bus;
    uintptr_t capbase;
    uintptr_t opregs;
    uintptr_t rts;
    uintptr_t db;
    uint8_t num_ports;
    uint8_t max_slots;
    uint8_t running;
    uint32_t pgsz;             /* PAGESIZE 推导的最小支持页 */
    uint32_t ctxsz_cache;      /* HCCPARAMS1.CSZ 推导的上下文尺寸 */
    /* 事件环 */
    struct xhci_trb *event_trbs;
    struct xhci_erst_seg *erst;
    uint32_t event_ep;
    uint8_t event_cs;
    volatile uint32_t evt_busy;
    volatile uint32_t evt_pending;
    /* 命令环(一次一条, NetBSD sc_command_addr 语义) */
    struct xhci_ring cmd_ring;
    volatile uint32_t cmd_busy;
    volatile uint32_t cmd_done;
    uint32_t cmd_code;
    uint8_t cmd_slot_id;
    uint32_t cmd_trb_addr;     /* 已入队命令 TRB 的物理地址(完成配对) */
    /* DCBAA + slots */
    uint64_t *dcbaa;
    struct xhci_dev devs[XHCI_MAX_SLOTS];
    uint8_t port_slot[8];
};

static struct xhci_hcd g_xhci[CONFIG_USBHOST_MAX_BUS];

/* 端口事件序号: ISR 每吞掉一个 Port Status Change 事件加一。适配层的
 * 热插拔看门狗轮询它决定是否唤醒 hub 线程(vendored 的 roothub_intbuf
 * 永不清零, 缓冲内容区分不了"已处理"与"新事件", 序号可以)。 */
volatile uint32_t g_xhci_port_evt_seq;

/* ===================== 寄存器访问 ===================== */

static inline uint32_t xhci_r32(struct xhci_hcd *hcd, uintptr_t off)
{
    return *(volatile uint32_t *)(hcd->opregs + off);
}

static inline void xhci_w32(struct xhci_hcd *hcd, uintptr_t off, uint32_t val)
{
    *(volatile uint32_t *)(hcd->opregs + off) = val;
}

static inline uint32_t xhci_r32_cap(struct xhci_hcd *hcd, uintptr_t off)
{
    return *(volatile uint32_t *)(hcd->capbase + off);
}

static inline uint32_t xhci_r32_rts(struct xhci_hcd *hcd, uintptr_t off)
{
    return *(volatile uint32_t *)(hcd->rts + off);
}

static inline void xhci_w32_rts(struct xhci_hcd *hcd, uintptr_t off, uint32_t val)
{
    *(volatile uint32_t *)(hcd->rts + off) = val;
}

static inline uint32_t xhci_portsc(struct xhci_hcd *hcd, uint32_t port)
{
    return xhci_r32(hcd, XHCI_PORTSC(port));
}

static inline void xhci_portsc_w(struct xhci_hcd *hcd, uint32_t port, uint32_t val)
{
    xhci_w32(hcd, XHCI_PORTSC(port), val);
}

/* ===================== 缓存一致性 ===================== */

static inline void xhci_dcache_clean(void *addr, uint32_t size)
{
    extern void usb_dcache_clean(uintptr_t addr, size_t size);
    usb_dcache_clean((uintptr_t)addr, size);
}

static inline void xhci_dcache_invalidate(void *addr, uint32_t size)
{
    extern void usb_dcache_invalidate(uintptr_t addr, size_t size);
    usb_dcache_invalidate((uintptr_t)addr, size);
}

/* ===================== DCI / 上下文寻址 ===================== */

/* xHCI spec §4.5.2: ep0=1, OUT n=2n, IN n=2n+1 */
static inline uint32_t xhci_dci_of(const struct usb_endpoint_descriptor *ep)
{
    uint32_t num = ep->bEndpointAddress & USB2_EP_NUM_MASK;

    return ((ep->bEndpointAddress & USB2_DIR_IN) != 0U) ? num * 2U + 1U
                                                        : num * 2U;
}

/* EP context type: control/isoc=0 基础 +4(控制或 IN) */
static inline uint32_t xhci_ep_type_of(const struct usb_endpoint_descriptor *ep,
                                       uint8_t is_control)
{
    uint32_t attr = ep->bmAttributes & 0x3U;
    uint32_t dir_in = (ep->bEndpointAddress & USB2_DIR_IN) != 0U;
    uint32_t type;

    if (is_control) {
        type = EP_TYPE_CONTROL;
    } else if (attr == 0U) {
        type = dir_in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT;
    } else {
        type = dir_in ? EP_TYPE_INTR_IN : EP_TYPE_INTR_OUT;
    }
    return type;
}

static inline uint32_t *xhci_in_ctx(struct xhci_dev *dev, uint32_t dci)
{
    return (uint32_t *)(dev->input_ctx + dev->ctxsz * (dci + 1U));
}

static inline uint32_t *xhci_out_ctx(struct xhci_dev *dev, uint32_t dci)
{
    return (uint32_t *)(dev->dev_ctx + dev->ctxsz * dci);
}

/* ===================== 环管理 ===================== */

static int xhci_ring_init(struct xhci_ring *ring, uint32_t num_trbs)
{
    ring->trbs = usb_sys_malloc_align(64U, num_trbs * TRB_SIZE);
    ring->tds = usb_sys_malloc_align(64U, num_trbs * sizeof(struct xhci_td));
    if (ring->trbs == NULL || ring->tds == NULL) {
        return -USB_ERR_NOMEM;
    }
    memset(ring->trbs, 0, num_trbs * TRB_SIZE);
    memset(ring->tds, 0, num_trbs * sizeof(struct xhci_td));
    ring->num_trbs = num_trbs;
    ring->ep = 0;
    ring->cs = 1;
    ring->dequeue = 0;
    /* 不预写 LINK TRB: 到达环尾时随批次写入(NetBSD xhci_ring_put)。 */
    return 0;
}

static uint32_t xhci_ring_space(struct xhci_ring *ring)
{
    uint32_t used = (ring->ep - ring->dequeue + ring->num_trbs) % ring->num_trbs;

    return ring->num_trbs - 2U - used;
}

/*
 * 批量入队(NetBSD xhci_ring_put 语义): 依次写各 TRB, 环尾处写 LINK(TRB 环
 * 回 + TC 位)并翻转 cs; 批内第一个 TRB 先以翻转的 cycle 写入(硬件不可见),
 * 其余按各位置的 cycle 写入, 最后才把首 TRB 置回正确 cycle —— 控制器可能
 * 在门铃前预取, 不得看到半条链。td 记入批内每个槽(事件落在任何 TRB 都能
 * 找回)。返回首 TRB 索引。
 */
static int xhci_ring_put(struct xhci_ring *ring, const struct xhci_trb *trbs,
                         uint32_t ntrbs, struct xhci_td *td)
{
    uint32_t ri = ring->ep;
    uint8_t cs = ring->cs;
    uint32_t first_slot = 0xFFFFFFFFU;
    uint8_t first_cs = 0U;
    uint32_t i;

    if (xhci_ring_space(ring) < ntrbs + 1U) {
        return -1;
    }

    for (i = 0U; i < ntrbs; i++) {
        struct xhci_trb t = trbs[i];
        struct xhci_td td_store = *td;

        if (ri == ring->num_trbs - 1U) {
            /* LINK TRB: 环回环基址, TC 位翻转消费者 cycle */
            ring->trbs[ri].dw0 = (uint32_t)(uintptr_t)ring->trbs;
            ring->trbs[ri].dw1 = 0;
            ring->trbs[ri].dw2 = 0;
            ring->trbs[ri].dw3 = TRB3_TYPE_SET(TRB_TYPE_LINK) | TRB3_ENT | cs;
            ring->tds[ri].urb = NULL;
            ri = 0;
            cs ^= 1U;
            continue;
        }
        if (first_slot == 0xFFFFFFFFU) {
            /* 首 TRB: 记下它所在位置的 cycle, 先以翻转值写入(硬件不可见) */
            first_slot = ri;
            first_cs = cs;
            t.dw3 = (t.dw3 & ~TRB3_CYCLE) | ((~cs) & 1U);
        } else {
            t.dw3 = (t.dw3 & ~TRB3_CYCLE) | cs;
        }
        ring->trbs[ri] = t;
        /* tds 存的是绝对环下标语义(first/last): complete_td 拿事件下标与
         * last_idx 比较判 status 段, 并把终态镜像回 first_idx 槽 - 调用方
         * 留下的偏移/默认值都会让轮询等不到完成(2026-09-20 板验) */
        td_store.first_idx = first_slot;
        td_store.last_idx = ri;
        ring->tds[ri] = td_store;
        ri++;
    }

    if (first_slot != 0xFFFFFFFFU) {
        /* 批次全部落定后才把首 TRB 置回正确 cycle => 整链原子生效 */
        ring->trbs[first_slot].dw3 =
            (ring->trbs[first_slot].dw3 & ~TRB3_CYCLE) | first_cs;
    }
    ring->ep = ri;
    ring->cs = cs;
    return (int)first_slot;
}

/* ===================== 事件处理(前置声明) ===================== */
static void xhci_event_process(struct xhci_hcd *hcd);

/* ===================== 命令提交 ===================== */

/*
 * 单命令路径(NetBSD xhci_do_command_locked 的同步版): 一次一条命令, 完成
 * 事件按 TRB 物理地址配对; 超时走 NetBSD xhci_abort_command(CA + 等 CRR 清 +
 * 命令环复位重挂)。
 */
static int xhci_do_command(struct xhci_hcd *hcd, const struct xhci_trb *trb,
                           uint32_t timeout_ms)
{
    struct xhci_td td;
    struct xhci_trb cmd = *trb;
    int idx;
    uint32_t t;

    if (hcd->cmd_busy != 0U) {
        return -USB_ERR_BUSY;
    }
    hcd->cmd_busy = 1U;
    hcd->cmd_done = 0U;
    hcd->cmd_code = 0U;
    hcd->cmd_slot_id = 0U;

    memset(&td, 0, sizeof(td));
    idx = xhci_ring_put(&hcd->cmd_ring, &cmd, 1U, &td);
    if (idx < 0) {
        hcd->cmd_busy = 0U;
        return -USB_ERR_NOMEM;
    }
    hcd->cmd_trb_addr = (uint32_t)(uintptr_t)&hcd->cmd_ring.trbs[idx];
    xhci_dcache_clean(hcd->cmd_ring.trbs, hcd->cmd_ring.num_trbs * TRB_SIZE);
    *(volatile uint32_t *)hcd->db = 0U; /* doorbell 0, target 0 = 命令环 */

    t = 0U;
    while (t < timeout_ms) {
        xhci_event_process(hcd);
        if (hcd->cmd_done != 0U) {
            break;
        }
        usb_osal_msleep(1);
        t++;
    }
    if (hcd->cmd_done == 0U) {
        /* NetBSD xhci_abort_command: 写 CRCR.CA, 等 CRR 自清, 复位环 */
        uint32_t crcr = xhci_r32(hcd, XHCI_CRCR_LO);
        uint32_t i;

        xhci_w32(hcd, XHCI_CRCR_LO, crcr | XHCI_CRCR_LO_CA);
        for (i = 0U; i < 500U; i++) {
            if ((xhci_r32(hcd, XHCI_CRCR_LO) & XHCI_CRCR_LO_CRR) == 0U) {
                break;
            }
            usb_osal_msleep(1);
        }
        hcd->cmd_ring.ep = 0;
        hcd->cmd_ring.cs = 1;
        xhci_w32(hcd, XHCI_CRCR_LO,
                 (uint32_t)(uintptr_t)hcd->cmd_ring.trbs | XHCI_CRCR_LO_RCS);
        hcd->cmd_busy = 0U;
        USB_LOG_ERR("command timeout (type=%u)\r\n", TRB3_TYPE_GET(cmd.dw3));
        return -USB_ERR_TIMEOUT;
    }
    hcd->cmd_busy = 0U;
    if (hcd->cmd_code != TRB_CODE_SUCCESS) {
        return -USB_ERR_IO;
    }
    return 0;
}

static int xhci_cmd_enable_slot(struct xhci_hcd *hcd, uint8_t *slot_id)
{
    struct xhci_trb trb;
    int ret;

    memset(&trb, 0, sizeof(trb));
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_ENABLE_SLOT);
    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    *slot_id = hcd->cmd_slot_id;
    return 0;
}

/* NetBSD xhci_host_dequeue: 重编程 ep 上下文前把传输环归零(TRB/td 全清,
 * ep=0, cs=1) - 否则已消费的 TRB 带着 cycle=1 在重臂的 dequeue 上"复活",
 * 控制器会重放旧 TD(2026-09-20: SET_ADDRESS Transaction Error 的根因) */
static void xhci_ring_reset(struct xhci_ring *ring)
{
    memset(ring->trbs, 0, ring->num_trbs * TRB_SIZE);
    memset(ring->tds, 0, ring->num_trbs * sizeof(struct xhci_td));
    ring->ep = 0;
    ring->cs = 1;
    ring->dequeue = 0;
}

/*
 * 输入上下文构建(NetBSD xhci_setup_ctx 的 ep0 情形): 控制上下文 drop=0 /
 * add=A0|A1(spec: drop 在 dw0, add 在 dw1); slot ctx 只填 route/speed/
 * ctx_entries + root port, dw3 rsvdZ; ep0 ctx: type=control, CERR=3,
 * MPS, TR dequeue=环基址|DCS=1, avg TRB len 8 (spec §6.2.3)。
 */
static void xhci_setup_ep0_input_ctx(struct xhci_dev *dev, uint16_t mps)
{
    uint32_t words = dev->ctxsz / 4U;
    uint32_t *icc = (uint32_t *)dev->input_ctx;
    uint32_t *slot = xhci_in_ctx(dev, 0);
    uint32_t *ep0 = xhci_in_ctx(dev, 1);
    uint32_t speed = SLOT_CTX_SPEED_HS;

    xhci_ring_reset(dev->ep_rings[1]);

    memset(dev->input_ctx, 0, words * (2U + XHCI_MAX_DCI) * 4U);
    icc[1] = (1U << 0) | (1U << 1); /* Add: slot + ep0 */

    if (dev->hport->speed == USB_SPEED_LOW) {
        speed = SLOT_CTX_SPEED_LS;
    } else if (dev->hport->speed == USB_SPEED_FULL) {
        speed = SLOT_CTX_SPEED_FS;
    }
    slot[0] = SLOT_CTX_DEV_INFO_LAST_CTX(1) |
              SLOT_CTX_DEV_INFO_SPEED(speed) |
              SLOT_CTX_DEV_INFO_ROUTE(0);
    slot[1] = SLOT_CTX_DEV_INFO2_PORT(dev->hport->port) |
              SLOT_CTX_DEV_INFO2_MAX_EXIT(0);
    /* slot[2] = TT/hub 字段: 直连根口全 0; slot[3] = rsvdZ(输入上下文) */

    ep0[0] = EP_CTX_0_INTERVAL_SET(0);
    ep0[1] = EP_CTX_1_EP_TYPE_SET(EP_TYPE_CONTROL) |
             EP_CTX_1_CERR_SET(3) |
             EP_CTX_1_MAX_PACKET_SET(mps);
    ep0[2] = ((uint32_t)(uintptr_t)dev->ep_rings[1]->trbs &
              ~0xFU) | EP_CTX_2_CYCLE;
    ep0[3] = 0; /* TR Dequeue Hi: 本平台 DDR < 4GB */
    ep0[4] = EP_CTX_4_AVG_TRB_LEN(8);

    xhci_dcache_clean(dev->input_ctx, words * (2U + XHCI_MAX_DCI) * 4U);
}

static int xhci_cmd_address_device(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                   uint8_t bsr, uint8_t usb_addr, uint16_t mps)
{
    struct xhci_trb trb;
    int ret;

    xhci_setup_ep0_input_ctx(dev, mps);
    if (bsr == 0U) {
        /* 本机 DWC3 实测(2026-09-20 板验): BSR=0 时 SET_ADDRESS 线上
         * token 的新地址就取自输入 slot ctx 的地址字段 - 设备还在 0 时
         * 写 2, token 打到 2 上三连 Transaction Error; 写 0 则控制器自动
         * 分配地址(1 起), 线上 SET_ADDRESS(分配值) 设备应答, 分配值写回
         * 输出上下文。这里恒写 0 让控制器分配, 分配值以输出上下文为准
         * (栈侧 dev_addr 只是簿记, 不参与 token)。 */
        xhci_in_ctx(dev, 0)[3] = 0;
        xhci_dcache_clean(dev->input_ctx, dev->ctxsz * (2U + XHCI_MAX_DCI));
    }

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)dev->input_ctx;
    trb.dw1 = 0;
    trb.dw2 = 0;
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_ADDRESS_DEV) | TRB3_SLOT_ID(dev->slot_id) |
              (bsr ? TRB3_BSR : 0U);

    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS);
    if (bsr == 0U) {
        /* 校验输出上下文: 控制器应已把 slot 置 ADDRESSED 并回报分配地址 */
        uint32_t dw3, st, hw_addr;

        xhci_dcache_invalidate(dev->dev_ctx, dev->ctxsz * (1U + XHCI_MAX_DCI));
        dw3 = xhci_out_ctx(dev, 0)[3];
        st = (dw3 >> 27) & 0x1FU;
        hw_addr = dw3 & 0xFFU;
        usbh_console_printf("[USBH] assigned usb addr %u (state=%u)\r\n",
                            hw_addr, st);
        if (st == SLOT_CTX_STATE_ADDRESSED && hw_addr != 0U) {
            ret = 0;
        } else if (ret == 0) {
            ret = -USB_ERR_IO;
        }
    }
    if (ret != 0) {
        USB_LOG_ERR("Address Device (bsr=%u addr=%u) failed, code=%u\r\n",
                    bsr, usb_addr, hcd->cmd_code);
        return ret;
    }
    dev->ep0_mps_hw = mps;
    return 0;
}

static int xhci_cmd_evaluate_context(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                     uint16_t mps)
{
    struct xhci_trb trb;
    int ret;

    xhci_setup_ep0_input_ctx(dev, mps);

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)dev->input_ctx;
    trb.dw1 = 0;
    trb.dw2 = 0;
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_EVALUATE_CTX) | TRB3_SLOT_ID(dev->slot_id);

    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS);
    if (ret != 0) {
        USB_LOG_ERR("Evaluate Context failed, code=%u\r\n", hcd->cmd_code);
        return ret;
    }
    dev->ep0_mps_hw = mps;
    return 0;
}

/* 配置非 ep0 端点(首次使用)。interval/burst 按 CherryUSB urb 描述取值。 */
static int xhci_cmd_configure_ep(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                 uint32_t dci, const struct usb_endpoint_descriptor *ep,
                                 struct xhci_ring *ring)
{
    uint32_t words = dev->ctxsz / 4U;
    uint32_t *icc = (uint32_t *)dev->input_ctx;
    uint32_t *slot = xhci_in_ctx(dev, 0);
    uint32_t *epc = xhci_in_ctx(dev, dci);
    uint32_t speed = SLOT_CTX_SPEED_HS;
    uint16_t ep_mps = ep->wMaxPacketSize & 0x7FFU;
    uint8_t is_intr = ((ep->bmAttributes & 0x3U) == 0x3U);
    uint8_t interval = (is_intr && ep->bInterval > 0U) ? (ep->bInterval - 1U) : 0U;
    uint8_t burst = (uint8_t)((ep->wMaxPacketSize >> 11) & 0x3U);
    struct xhci_trb trb;
    int ret;

    memset(dev->input_ctx, 0, words * (2U + XHCI_MAX_DCI) * 4U);
    icc[1] = (1U << 0) | (1U << dci); /* Add: slot + ep */

    if (dev->hport->speed == USB_SPEED_LOW) {
        speed = SLOT_CTX_SPEED_LS;
    } else if (dev->hport->speed == USB_SPEED_FULL) {
        speed = SLOT_CTX_SPEED_FS;
    }
    slot[0] = SLOT_CTX_DEV_INFO_LAST_CTX(dci) |
              SLOT_CTX_DEV_INFO_SPEED(speed) |
              SLOT_CTX_DEV_INFO_ROUTE(0);
    slot[1] = SLOT_CTX_DEV_INFO2_PORT(dev->hport->port) |
              SLOT_CTX_DEV_INFO2_MAX_EXIT(0);
    slot[3] = SLOT_CTX_DEV_STATE_ADDR(dev->hport->dev_addr) |
              SLOT_CTX_DEV_STATE_SLOT_STATE(SLOT_CTX_STATE_CONFIGURED);

    epc[0] = EP_CTX_0_INTERVAL_SET(interval);
    epc[1] = EP_CTX_1_EP_TYPE_SET(xhci_ep_type_of(ep, 0)) |
             EP_CTX_1_CERR_SET(3) |
             EP_CTX_1_MAX_PACKET_SET(ep_mps) |
             EP_CTX_1_MAX_BURST_SET(burst);
    epc[2] = ((uint32_t)(uintptr_t)ring->trbs & ~0xFU) | EP_CTX_2_CYCLE;
    epc[3] = 0; /* TR Dequeue Hi: 本平台 DDR < 4GB */
    /* avg TRB len: bulk = MPS, 其余 8 (spec §6.2.3 提示值, word4) */
    epc[4] = EP_CTX_4_AVG_TRB_LEN(((ep->bmAttributes & 0x3U) == 0U) ? ep_mps : 8U);

    xhci_dcache_clean(dev->input_ctx, words * (2U + XHCI_MAX_DCI) * 4U);

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)dev->input_ctx;
    trb.dw1 = 0;
    trb.dw2 = 0;
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_CONFIGURE_EP) | TRB3_SLOT_ID(dev->slot_id);

    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS);
    if (ret != 0) {
        USB_LOG_ERR("Configure EP (dci=%u) failed, code=%u\r\n", dci,
                    hcd->cmd_code);
        return ret;
    }
    dev->ep_configured |= (1U << dci);
    return 0;
}

/* ===================== 槽位生命周期 ===================== */

static struct xhci_dev *xhci_find_dev(struct xhci_hcd *hcd, uint8_t slot_id)
{
    if (slot_id == 0U || slot_id >= hcd->max_slots) {
        return NULL;
    }
    if (!hcd->devs[slot_id].in_use) {
        return NULL;
    }
    return &hcd->devs[slot_id];
}

static struct xhci_dev *xhci_get_dev(struct xhci_hcd *hcd, struct usbh_hubport *hport)
{
    uint8_t port = hport->port;

    if (port >= 1U && port <= hcd->num_ports && hcd->port_slot[port] != 0U) {
        return xhci_find_dev(hcd, hcd->port_slot[port]);
    }
    return NULL;
}

/*
 * 首次为 hport 建槽: Enable Slot -> 页式 in/out ctx -> DCBAA[slot] -> ep0
 * 环 -> Address Device BSR=1(addr 0 可达; CherryUSB 先读 8 字节描述符再
 * SET_ADDRESS, 故这里不能像 NetBSD 一样单步 BSR=0)。MPS 取 CherryUSB 按
 * 速度预设的 ep0 默认值(HS/FS=64)。
 */
static int xhci_setup_slot(struct xhci_hcd *hcd, struct usbh_hubport *hport)
{
    struct xhci_dev *dev;
    uint8_t slot_id = 0U;
    uint16_t mps = hport->ep0.wMaxPacketSize;
    int ret;

    if (hport->port < 1U || hport->port > hcd->num_ports) {
        return -USB_ERR_INVAL;
    }
    ret = xhci_cmd_enable_slot(hcd, &slot_id);
    if (ret != 0) {
        USB_LOG_ERR("Enable slot failed, ret=%d\r\n", ret);
        return ret;
    }
    usbh_console_printf("[USBH] xHCI slot %u enabled\r\n", slot_id);

    dev = &hcd->devs[slot_id];
    memset(dev, 0, sizeof(*dev));
    dev->in_use = 1U;
    dev->slot_id = slot_id;
    dev->hport = hport;
    dev->ctxsz = hcd->ctxsz_cache;
    dev->input_ctx = usb_sys_malloc_align(64U, dev->ctxsz * (2U + XHCI_MAX_DCI));
    dev->dev_ctx = usb_sys_malloc_align(64U, dev->ctxsz * (1U + XHCI_MAX_DCI));
    if (dev->input_ctx == NULL || dev->dev_ctx == NULL) {
        dev->in_use = 0U;
        return -USB_ERR_NOMEM;
    }
    memset(dev->input_ctx, 0, dev->ctxsz * (2U + XHCI_MAX_DCI));
    memset(dev->dev_ctx, 0, dev->ctxsz * (1U + XHCI_MAX_DCI));

    /* DCBAA[slot] = 输出设备上下文 */
    hcd->dcbaa[slot_id] = (uint64_t)(uintptr_t)dev->dev_ctx;
    xhci_dcache_clean(hcd->dcbaa, sizeof(uint64_t) * hcd->max_slots);
    xhci_dcache_clean(dev->dev_ctx, dev->ctxsz * (1U + XHCI_MAX_DCI));

    /* ep0 传输环(dci=1) */
    dev->ep_rings[1] = usb_sys_malloc_align(64U, sizeof(struct xhci_ring));
    if (dev->ep_rings[1] == NULL) {
        dev->in_use = 0U;
        return -USB_ERR_NOMEM;
    }
    ret = xhci_ring_init(dev->ep_rings[1], XHCI_TRANSFER_RING_SIZE);
    if (ret != 0) {
        dev->in_use = 0U;
        return ret;
    }

    ret = xhci_cmd_address_device(hcd, dev, 1U, 0U, mps);
    if (ret != 0) {
        dev->in_use = 0U;
        return ret;
    }

    hcd->port_slot[hport->port] = slot_id;
    return 0;
}

/* ===================== 控制器初始化 ===================== */

/* hc_reset(NetBSD xhci_hc_reset, 逐行): CNR -> halt -> HCRST 自清 -> CNR */
static int xhci_hc_reset(struct xhci_hcd *hcd)
{
    uint32_t usbcmd, usbsts;
    uint32_t i;

    for (i = 0U; i < 100U; i++) {
        usbsts = xhci_r32(hcd, XHCI_USBSTS);
        if ((usbsts & XHCI_STS_CNR) == 0U) {
            break;
        }
        usb_osal_msleep(1);
    }
    if (i >= 100U) {
        USB_LOG_ERR("controller not ready timeout\r\n");
        return -USB_ERR_TIMEOUT;
    }

    xhci_w32(hcd, XHCI_USBCMD, 0U);
    usb_osal_msleep(1);

    xhci_w32(hcd, XHCI_USBCMD, XHCI_CMD_HCRST);
    for (i = 0U; i < 100U; i++) {
        /* 先等 1ms 再读(Intel errata 同款节拍, NetBSD 保留) */
        usb_osal_msleep(1);
        usbcmd = xhci_r32(hcd, XHCI_USBCMD);
        if ((usbcmd & XHCI_CMD_HCRST) == 0U) {
            break;
        }
    }
    if (i >= 100U) {
        USB_LOG_ERR("host controller reset timeout\r\n");
        return -USB_ERR_TIMEOUT;
    }

    for (i = 0U; i < 100U; i++) {
        usbsts = xhci_r32(hcd, XHCI_USBSTS);
        if ((usbsts & XHCI_STS_CNR) == 0U) {
            break;
        }
        usb_osal_msleep(1);
    }
    if (i >= 100U) {
        USB_LOG_ERR("controller not ready timeout after reset\r\n");
        return -USB_ERR_TIMEOUT;
    }
    return 0;
}

static int xhci_controller_init(struct xhci_hcd *hcd)
{
    uint32_t cap, hcs1, hcs2, hcc1;
    uint32_t caplen, hciver;
    uint32_t rts_off, db_off;
    uint32_t i;
    uint32_t num_sp;
    int ret;

    /* 能力解码(DWC3 窗口 32 位访问 + 本板禁非对齐 MMIO: CAPLENGTH 与
     * HCIVERSION 同在偏移 0 的一个字里, 一次对齐读取两个字段) */
    cap = xhci_r32_cap(hcd, XHCI_CAPLENGTH);
    caplen = cap & 0xFFU;
    hcd->opregs = hcd->capbase + caplen;
    hciver = (cap >> 16) & 0xFFFFU;
    if (hciver < XHCI_HCIVERSION_0_96 || hciver >= 0x0200U) {
        usbh_console_printf("[USBH] warning: HCIVERSION 0x%04x\r\n", hciver);
    }
    hcs1 = xhci_r32_cap(hcd, XHCI_HCSPARAMS1);
    hcs2 = xhci_r32_cap(hcd, XHCI_HCSPARAMS2);
    hcc1 = xhci_r32_cap(hcd, XHCI_HCCPARAMS1);

    /* 上下文尺寸(NetBSD sc_ctxsz): CSZ=1 -> 64B, 否则 32B */
    hcd->ctxsz_cache = (XHCI_HCCPARAMS1_CSZ(hcc1) != 0U) ? 64U : 32U;

    hcd->max_slots = (uint8_t)XHCI_HCS1_DEVSLOT_MAX(hcs1);
    hcd->num_ports = (uint8_t)XHCI_HCS1_N_PORTS(hcs1);
    db_off = xhci_r32_cap(hcd, XHCI_DBOFF) & ~0xFU;
    rts_off = xhci_r32_cap(hcd, XHCI_RTSOFF) & ~0xFU;
    hcd->db = hcd->capbase + db_off;
    hcd->rts = hcd->capbase + rts_off;

    if (hcd->num_ports > 8U) {
        hcd->num_ports = 8U;
    }
    if (hcd->max_slots > XHCI_MAX_SLOTS) {
        hcd->max_slots = XHCI_MAX_SLOTS;
    }

    /* PAGESIZE: 取最小支持页(NetBSD sc_pgsz) */
    cap = xhci_r32(hcd, XHCI_PAGESIZE);
    if (cap == 0U) {
        USB_LOG_ERR("PAGESIZE reads 0\r\n");
        return -USB_ERR_INVAL;
    }
    for (i = 0U; i < 16U; i++) {
        if (cap & (1U << i)) {
            break;
        }
    }
    hcd->pgsz = 1U << (12U + i);

    usbh_console_printf(
        "[USBH] xHCI: %u ports, %u slots, ctxsz=%u pgsz=%u caplen=%u hcs1=0x%08x\r\n",
        hcd->num_ports, hcd->max_slots, hcd->ctxsz_cache, hcd->pgsz, caplen,
        hcs1);

    /* hc_reset 先于一切环/DCBAA 寄存器编程(NetBSD 顺序) */
    ret = xhci_hc_reset(hcd);
    if (ret != 0) {
        return ret;
    }

    /* 事件环 + 段表 */
    hcd->event_trbs = usb_sys_malloc_align(64U, XHCI_EVENT_RING_SIZE * TRB_SIZE);
    hcd->erst = usb_sys_malloc_align(64U, sizeof(struct xhci_erst_seg));
    if (hcd->event_trbs == NULL || hcd->erst == NULL) {
        return -USB_ERR_NOMEM;
    }
    memset(hcd->event_trbs, 0, XHCI_EVENT_RING_SIZE * TRB_SIZE);
    memset(hcd->erst, 0, sizeof(struct xhci_erst_seg));
    hcd->erst->seg_base_lo = (uint32_t)(uintptr_t)hcd->event_trbs;
    hcd->erst->seg_base_hi = 0;
    hcd->erst->seg_size = XHCI_EVENT_RING_SIZE;
    hcd->event_ep = 0;
    hcd->event_cs = 1;
    hcd->evt_busy = 0;
    hcd->evt_pending = 0;
    xhci_dcache_clean(hcd->event_trbs, XHCI_EVENT_RING_SIZE * TRB_SIZE);
    xhci_dcache_clean(hcd->erst, sizeof(struct xhci_erst_seg));

    /* 命令环 */
    ret = xhci_ring_init(&hcd->cmd_ring, XHCI_CMD_RING_SIZE);
    if (ret != 0) {
        return ret;
    }

    /* DCBAA((1+maxslots)*8B) + scratchpad(DCBAA[0]=数组指针, 数组与缓冲页
     * 对齐; scratchpad 有效是 Address Device 的硬前提, 缺它报 Parameter
     * Error 17 - 数组本身也要 flush, 只 flush DCBAA 不够, 2026-09-20 板验) */
    hcd->dcbaa = usb_sys_malloc_align(64U, sizeof(uint64_t) * (hcd->max_slots + 1U));
    if (hcd->dcbaa == NULL) {
        return -USB_ERR_NOMEM;
    }
    memset(hcd->dcbaa, 0, sizeof(uint64_t) * (hcd->max_slots + 1U));

    num_sp = XHCI_HCS2_SPB_MAX(hcs2);
    if (num_sp > 0U) {
        uint64_t *sp_array = usb_sys_malloc_align(64U, sizeof(uint64_t) * num_sp);
        uint8_t *sp_buf = usb_sys_malloc_align(hcd->pgsz, hcd->pgsz * num_sp);

        if (sp_array == NULL || sp_buf == NULL) {
            return -USB_ERR_NOMEM;
        }
        memset(sp_array, 0, sizeof(uint64_t) * num_sp);
        memset(sp_buf, 0, hcd->pgsz * num_sp);
        for (i = 0U; i < num_sp; i++) {
            sp_array[i] = (uint64_t)(uintptr_t)(sp_buf + i * hcd->pgsz);
        }
        hcd->dcbaa[0] = (uint64_t)(uintptr_t)sp_array;
        xhci_dcache_clean(sp_array, sizeof(uint64_t) * num_sp);
        xhci_dcache_clean(sp_buf, hcd->pgsz * num_sp);
    }
    xhci_dcache_clean(hcd->dcbaa, sizeof(uint64_t) * (hcd->max_slots + 1U));

    /* 寄存器编程(NetBSD xhci_init 1569-1684 的顺序) */
    xhci_w32_rts(hcd, XHCI_ERSTSZ(0), XHCI_ERSTS_SET(1));
    xhci_w32_rts(hcd, XHCI_ERSTBA_LO(0), (uint32_t)(uintptr_t)hcd->erst);
    xhci_w32_rts(hcd, XHCI_ERSTBA_HI(0), 0U);
    xhci_w32_rts(hcd, XHCI_ERDP_LO(0),
                 ((uint32_t)(uintptr_t)hcd->event_trbs & ~0xFU) | XHCI_ERDP_BUSY);
    xhci_w32_rts(hcd, XHCI_ERDP_HI(0), 0U);

    xhci_w32(hcd, XHCI_DCBAAP_LO, (uint32_t)(uintptr_t)hcd->dcbaa);
    xhci_w32(hcd, XHCI_DCBAAP_HI, 0U);
    xhci_w32(hcd, XHCI_CRCR_HI, 0U);
    xhci_w32(hcd, XHCI_CRCR_LO,
             (uint32_t)(uintptr_t)hcd->cmd_ring.trbs | XHCI_CRCR_LO_RCS);

    /* CONFIG: MaxSlotsEn 读改写(保留 U3E/CIE), 使能全部槽位 */
    cap = xhci_r32(hcd, XHCI_CONFIG);
    cap = (cap & ~XHCI_CONFIG_SLOTS_MASK) | (hcd->max_slots & XHCI_CONFIG_SLOTS_MASK);
    xhci_w32(hcd, XHCI_CONFIG, cap);

    /* DNCTRL 复位值即 0(NetBSD 不写), 显式清一次求确定 */
    xhci_w32(hcd, XHCI_DNCTRL, 0U);

    /* 中断: IMAN 仅 ENA, IMOD=0(非 Intel 无 moderation, NetBSD xhci_start) */
    xhci_w32_rts(hcd, XHCI_IMOD(0), 0U);
    xhci_w32_rts(hcd, XHCI_IMAN(0), XHCI_IMAN_INTR_ENA);

    /* RUN: RS|INTE(NetBSD 不置 HSEE) */
    xhci_w32(hcd, XHCI_USBCMD, XHCI_CMD_RS | XHCI_CMD_INTE);
    for (i = 0U; i < 1000U; i++) {
        if ((xhci_r32(hcd, XHCI_USBSTS) & XHCI_STS_HCH) == 0U) {
            break;
        }
        usb_osal_msleep(1);
    }
    if (i >= 1000U) {
        USB_LOG_ERR("xHCI run timeout\r\n");
        return -USB_ERR_TIMEOUT;
    }

    /* 端口上电(NetBSD 无此步 - 它的 usbdi 会发 SetFeature(PORT_POWER);
     * vendored hub 类从不给 roothub 发 POWER, 保留这一遍兜底) */
    for (i = 1U; i <= hcd->num_ports; i++) {
        uint32_t ps = xhci_portsc(hcd, i);
        uint32_t try;

        if ((ps & XHCI_PS_PP) != 0U) {
            continue;
        }
        for (try = 0U; try < 5U; try++) {
            xhci_portsc_w(hcd, i, (ps & ~XHCI_PS_PED) | XHCI_PS_PP);
            usb_osal_msleep(10);
            if ((xhci_portsc(hcd, i) & XHCI_PS_PP) != 0U) {
                break;
            }
        }
        usbh_console_printf("[USBH] port %u PORTSC=0x%08x\r\n", i,
                            xhci_portsc(hcd, i));
    }

    hcd->running = 1U;
    return 0;
}

/* ===================== usb_hc.h 接口 ===================== */

int usb_hc_init(struct usbh_bus *bus)
{
    struct xhci_hcd *hcd;
    uint8_t busid = bus->busid;
    int ret;

    if (busid >= CONFIG_USBHOST_MAX_BUS) {
        return -USB_ERR_INVAL;
    }
    hcd = &g_xhci[busid];
    memset(hcd, 0, sizeof(*hcd));
    hcd->bus = bus;
    hcd->capbase = usb_hc_get_register_base(busid);
    if (hcd->capbase == 0U) {
        USB_LOG_ERR("Invalid register base for bus %u\r\n", busid);
        return -USB_ERR_INVAL;
    }

    /* 入口期读数(任何平台写之前): 诊断基线 */
    usbh_console_printf("[USBH] entry: cap[0]=0x%08x GSNPSID=0x%08x\r\n",
                        *(volatile uint32_t *)hcd->capbase,
                        *(volatile uint32_t *)(hcd->capbase + DWC3_GSNPSID));

    /* 平台层(D38: NetBSD rk_usb2phy + dwc3_fdt 全序, glue 内 once-guard)
     * + nports 发布 + 中断安装 */
    usb_hc_low_level_init(bus);

    /* 平台序列后的核心态校验(GSNPSID/PRTCAP/孔径) */
    {
        volatile uint32_t *dwc3 = (volatile uint32_t *)hcd->capbase;
        uint32_t revision = dwc3[DWC3_GSNPSID / 4U];
        uint32_t gctl = dwc3[DWC3_GCTL / 4U];

        if ((revision & DWC3_GSNPSID_MASK) != DWC3_GSNPSID_VAL) {
            USB_LOG_ERR("DWC3 not found, GSNPSID=0x%08x\r\n", revision);
            return -USB_ERR_INVAL;
        }
        if (((gctl & DWC3_GCTL_PRTCAP_MASK) >> 12) != DWC3_GCTL_PRTCAP_HOST) {
            USB_LOG_ERR("DWC3 not in host mode (GCTL=0x%08x)\r\n", gctl);
            return -USB_ERR_INVAL;
        }
        if ((dwc3[0] & 0xFFU) == 0U) {
            USB_LOG_ERR("xHCI aperture dead (cap=0)\r\n");
            return -USB_ERR_INVAL;
        }
    }

    ret = xhci_controller_init(hcd);
    if (ret != 0) {
        return ret;
    }

    usb_hc_enable_interrupt(busid);
    usbh_console_printf("[USBH] usb_hc_init done bus%u\r\n", busid);
    return 0;
}

int usb_hc_deinit(struct usbh_bus *bus)
{
    struct xhci_hcd *hcd;
    uint8_t busid = bus->busid;

    if (busid >= CONFIG_USBHOST_MAX_BUS) {
        return -USB_ERR_INVAL;
    }
    hcd = &g_xhci[busid];
    if (!hcd->running) {
        return 0;
    }
    usb_hc_disable_interrupt(busid);

    xhci_w32(hcd, XHCI_USBCMD, 0U);
    hcd->running = 0U;
    usb_hc_low_level_deinit(bus);
    return 0;
}

uint16_t usbh_get_frame_number(struct usbh_bus *bus)
{
    struct xhci_hcd *hcd = &g_xhci[bus->busid];

    if (!hcd->running) {
        return 0;
    }
    return (uint16_t)(xhci_r32_rts(hcd, XHCI_MFINDEX) & 0x3FFFU);
}

/* ===================== URB 提交 ===================== */

/* ep0 控制传输: SETUP(0-1 TRB) + DATA(0..n TRB) + STATUS(1 TRB)。
 * TRB 位取 NetBSD xhci_device_ctrl_start: SETUP 带 IDT+TRT(len>0), DATA 带
 * DIR_IN+ISP(IN)+CHAIN(分段时), STATUS 方向反转; IOC 只在 status(以及分段
 * data 的末块短路路径)。 */
static int xhci_queue_control(struct xhci_hcd *hcd, struct xhci_dev *dev,
                              struct usbh_urb *urb)
{
    struct usb_setup_packet *setup = urb->setup;
    struct xhci_ring *ring = dev->ep_rings[1];
    struct xhci_trb trbs[3];
    struct xhci_td td;
    uint8_t *buf = urb->transfer_buffer;
    uint32_t len = urb->transfer_buffer_length;
    uint32_t dir_in = (setup->bmRequestType & 0x80U) != 0U;
    int first_idx;
    uint32_t ntrbs = 0U;
    uint32_t tmo;

    if (urb->setup == NULL) {
        return -USB_ERR_INVAL;
    }
    if (len > TRB_MAX_BUFF_SIZE) {
        return -USB_ERR_INVAL; /* CherryUSB ep0 缓冲 <=2KB, 不会走到 */
    }
    {
        uintptr_t chk = (uintptr_t)buf;

        if ((chk & (TRB_MAX_BUFF_SIZE - 1U)) + len > TRB_MAX_BUFF_SIZE) {
            return -USB_ERR_INVAL; /* 跨界数据块不适合单 DATA TRB, 显式拒绝 */
        }
    }

    /* SETUP TRB */
    memset(&trbs[ntrbs], 0, sizeof(struct xhci_trb));
    trbs[ntrbs].dw0 = (uint32_t)setup->bmRequestType |
                      ((uint32_t)setup->bRequest << 8) |
                      ((uint32_t)setup->wValue << 16);
    trbs[ntrbs].dw1 = (uint32_t)setup->wIndex | ((uint32_t)setup->wLength << 16);
    trbs[ntrbs].dw2 = TRB2_LEN(8) | TRB2_IRQ(0);
    trbs[ntrbs].dw3 = TRB3_TYPE_SET(TRB_TYPE_SETUP) | TRB3_IDT;
    if (len > 0U) {
        trbs[ntrbs].dw3 |= dir_in ? TRB3_TRT_IN : TRB3_TRT_OUT;
    }
    ntrbs++;

    /* DATA TRB(len>0; 64KB 内单块) */
    if (len > 0U) {
        uintptr_t buf_addr = (uintptr_t)buf;

        if (!dir_in) {
            xhci_dcache_clean(buf, len);
        }
        memset(&trbs[ntrbs], 0, sizeof(struct xhci_trb));
        trbs[ntrbs].dw0 = (uint32_t)buf_addr;
        trbs[ntrbs].dw1 = (uint32_t)(buf_addr >> 32);
        trbs[ntrbs].dw2 = TRB2_LEN(len) | TRB2_IRQ(0);
        trbs[ntrbs].dw3 = TRB3_TYPE_SET(TRB_TYPE_DATA);
        if (dir_in) {
            trbs[ntrbs].dw3 |= TRB3_DIR_IN | TRB3_ISP;
        }
        ntrbs++;
    }

    /* STATUS TRB(方向与 data 相反) */
    memset(&trbs[ntrbs], 0, sizeof(struct xhci_trb));
    trbs[ntrbs].dw0 = 0;
    trbs[ntrbs].dw1 = 0;
    trbs[ntrbs].dw2 = TRB2_LEN(0) | TRB2_IRQ(0);
    trbs[ntrbs].dw3 = TRB3_TYPE_SET(TRB_TYPE_STATUS);
    if (!(dir_in && len > 0U)) {
        trbs[ntrbs].dw3 |= TRB3_DIR_IN;
    }
    trbs[ntrbs].dw3 |= TRB3_IOC;
    ntrbs++;

    if (xhci_ring_space(ring) < ntrbs + 1U) {
        return -USB_ERR_NOMEM;
    }
    memset(&td, 0, sizeof(td));
    td.urb = urb;
    td.kind = 1U;
    td.first_idx = 0U;
    td.last_idx = ntrbs - 1U;
    first_idx = xhci_ring_put(ring, trbs, ntrbs, &td);
    if (first_idx < 0) {
        return -USB_ERR_NOMEM;
    }
    xhci_dcache_clean(ring->trbs, ring->num_trbs * TRB_SIZE);
    *(volatile uint32_t *)(hcd->db + (dev->slot_id << 2)) = 1U; /* target dci 1 */

    /* 完成路径更新的是环数组里的 td 副本(ring->tds[每个 TRB 槽]), 等它,
     * 不是等栈上的 td */
    tmo = 0U;
    while (tmo < CTRL_TIMEOUT_MS) {
        xhci_event_process(hcd);
        if (ring->tds[first_idx].state != 0U) {
            break;
        }
        usb_osal_msleep(1);
        tmo++;
    }

    if (ring->tds[first_idx].state == 1U) {
        urb->actual_length = ring->tds[first_idx].actual;
        if (dir_in && urb->actual_length > 0U) {
            xhci_dcache_invalidate(buf, urb->actual_length);
        }
        return 0;
    }
    if (ring->tds[first_idx].state == 2U) {
        return ring->tds[first_idx].error;
    }
    return -USB_ERR_TIMEOUT;
}

/* bulk / interrupt: 单 NORMAL TD。>64KB 的 urb 按 64KB 边界分段并 CHAIN 成
 * 一个 TD(事件语义与 control 相同: 非末块事件只记账, 末块事件终态)。 */
static int xhci_queue_normal(struct xhci_hcd *hcd, struct xhci_dev *dev,
                             uint32_t dci, struct usbh_urb *urb)
{
    struct xhci_ring *ring = dev->ep_rings[dci];
    struct xhci_td td;
    uint8_t *buf = urb->transfer_buffer;
    uint32_t len = urb->transfer_buffer_length;
    uint32_t dir_in = (urb->ep->bEndpointAddress & USB2_DIR_IN) != 0U;
    uint32_t ntrbs = 0U;
    int first_idx;
    uint32_t tmo;
    uint32_t timeout_ms = urb->timeout;

    if (len == 0U) {
        ntrbs = 1U; /* ZLP: 空 NORMAL */
    } else {
        uintptr_t a = (uintptr_t)buf;
        uint32_t r = len;

        while (r > 0U) {
            uint32_t chunk = (r > TRB_MAX_BUFF_SIZE) ? TRB_MAX_BUFF_SIZE : r;

            if ((a & (TRB_MAX_BUFF_SIZE - 1U)) + chunk > TRB_MAX_BUFF_SIZE) {
                chunk = TRB_MAX_BUFF_SIZE - (a & (TRB_MAX_BUFF_SIZE - 1U));
            }
            ntrbs++;
            a += chunk;
            r -= chunk;
        }
    }
    if (ntrbs > 8U) {
        return -USB_ERR_INVAL; /* TD 上限(本栈 urb 不应超过 512KB) */
    }
    if (xhci_ring_space(ring) < ntrbs + 1U) {
        return -USB_ERR_NOMEM;
    }

    if (!dir_in && len > 0U) {
        xhci_dcache_clean(buf, len);
    }

    {
        struct xhci_trb trbs[8];
        uintptr_t a = (uintptr_t)buf;
        uint32_t r = len;
        uint32_t i = 0U;

        memset(trbs, 0, sizeof(trbs));
        if (len == 0U) {
            trbs[0].dw0 = 0;
            trbs[0].dw1 = 0;
            trbs[0].dw2 = TRB2_LEN(0) | TRB2_IRQ(0);
            trbs[0].dw3 = TRB3_TYPE_SET(TRB_TYPE_NORMAL) | TRB3_IOC;
            if (dir_in) {
                trbs[0].dw3 |= TRB3_ISP;
            }
        } else {
            while (r > 0U) {
                uint32_t chunk = (r > TRB_MAX_BUFF_SIZE) ? TRB_MAX_BUFF_SIZE : r;
                uint8_t last = 0U;

                if ((a & (TRB_MAX_BUFF_SIZE - 1U)) + chunk > TRB_MAX_BUFF_SIZE) {
                    chunk = TRB_MAX_BUFF_SIZE - (a & (TRB_MAX_BUFF_SIZE - 1U));
                }
                r -= chunk;
                last = (r == 0U) ? 1U : 0U;
                trbs[i].dw0 = (uint32_t)a;
                trbs[i].dw1 = (uint32_t)(a >> 32);
                trbs[i].dw2 = TRB2_LEN(chunk) | TRB2_IRQ(0);
                trbs[i].dw3 = TRB3_TYPE_SET(TRB_TYPE_NORMAL);
                if (!last) {
                    trbs[i].dw3 |= TRB3_CHAIN;
                } else {
                    trbs[i].dw3 |= TRB3_IOC;
                }
                if (dir_in) {
                    trbs[i].dw3 |= TRB3_ISP;
                }
                a += chunk;
                i++;
            }
        }

        memset(&td, 0, sizeof(td));
        td.urb = urb;
        td.kind = 0U;
        first_idx = xhci_ring_put(ring, trbs, ntrbs, &td);
        if (first_idx < 0) {
            return -USB_ERR_NOMEM;
        }
    }

    xhci_dcache_clean(ring->trbs, ring->num_trbs * TRB_SIZE);
    *(volatile uint32_t *)(hcd->db + (dev->slot_id << 2)) = dci;

    if (timeout_ms == 0U) {
        return 0; /* 异步: 完成由中断回调 */
    }

    /* 同 queue_control: 等环数组里的 td 副本 */
    tmo = 0U;
    while (tmo < timeout_ms) {
        xhci_event_process(hcd);
        if (ring->tds[first_idx].state != 0U) {
            break;
        }
        usb_osal_msleep(1);
        tmo++;
    }

    if (ring->tds[first_idx].state == 1U) {
        urb->actual_length = ring->tds[first_idx].actual;
        if (dir_in && urb->actual_length > 0U) {
            xhci_dcache_invalidate(buf, urb->actual_length);
        }
        return 0;
    }
    if (ring->tds[first_idx].state == 2U) {
        return ring->tds[first_idx].error;
    }
    return -USB_ERR_TIMEOUT;
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    struct usbh_hubport *hport;
    struct xhci_hcd *hcd;
    struct xhci_dev *dev;
    uint32_t dci;
    uint16_t mps;
    int ret;

    if (urb == NULL || urb->hport == NULL) {
        return -USB_ERR_INVAL;
    }
    hport = urb->hport;
    hcd = &g_xhci[hport->bus->busid];
    if (!hcd->running) {
        return -USB_ERR_SHUTDOWN;
    }

    dev = xhci_get_dev(hcd, hport);
    if (dev == NULL) {
        ret = xhci_setup_slot(hcd, hport);
        if (ret != 0) {
            return ret;
        }
        dev = xhci_get_dev(hcd, hport);
        if (dev == NULL) {
            return -USB_ERR_IO;
        }
    }

    if (urb->ep == &hport->ep0) {
        struct usb_setup_packet *setup = urb->setup;

        if (setup == NULL) {
            return -USB_ERR_INVAL;
        }

        /* SET_ADDRESS: xHCI 由 Address Device 命令完成, 不走 TRB。
         * 本机 DWC3 的 BSR=0 是"控制器自动分配地址"语义(见
         * xhci_cmd_address_device), 分配值与 CherryUSB 簿记的 dev_addr
         * 无关 - 栈侧照常记 2, 线上地址以控制器为准。 */
        if (setup->bRequest == USB_REQUEST_SET_ADDRESS) {
            uint16_t want = hport->ep0.wMaxPacketSize;

            if (want != dev->ep0_mps_hw) {
                ret = xhci_cmd_evaluate_context(hcd, dev, want);
                if (ret != 0) {
                    return ret;
                }
            }
            ret = xhci_cmd_address_device(hcd, dev, 0U,
                                          (uint8_t)(setup->wValue & 0xFFU),
                                          want);
            if (ret != 0) {
                return ret;
            }
            hport->dev_addr = (uint8_t)(setup->wValue & 0xFFU);
            urb->actual_length = 8; /* CherryUSB: 控制传输计入 setup 段 */
            return 0;
        }

        /* ep0 MPS 变化 -> Evaluate Context */
        mps = hport->ep0.wMaxPacketSize;
        if (mps != dev->ep0_mps_hw) {
            ret = xhci_cmd_evaluate_context(hcd, dev, mps);
            if (ret != 0) {
                return ret;
            }
        }

        return xhci_queue_control(hcd, dev, urb);
    }

    /* bulk / interrupt(iso 仍显式拒绝: G3/G4, M9 另案) */
    if ((urb->ep->bmAttributes & 0x3U) == 0x1U) {
        return -USB_ERR_NOTSUPP;
    }
    dci = xhci_dci_of(urb->ep);
    if (dci < 2U || dci >= XHCI_MAX_DCI) {
        return -USB_ERR_INVAL;
    }
    if (!(dev->ep_configured & (1U << dci))) {
        dev->ep_rings[dci] = usb_sys_malloc_align(64U, sizeof(struct xhci_ring));
        if (dev->ep_rings[dci] == NULL) {
            return -USB_ERR_NOMEM;
        }
        ret = xhci_ring_init(dev->ep_rings[dci], XHCI_TRANSFER_RING_SIZE);
        if (ret != 0) {
            return ret;
        }
        ret = xhci_cmd_configure_ep(hcd, dev, dci, urb->ep, dev->ep_rings[dci]);
        if (ret != 0) {
            return ret;
        }
    }

    return xhci_queue_normal(hcd, dev, dci, urb);
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    /* 同步模型下无在途中断传输; 异步传输由事件环完成/超时收尾。 */
    (void)urb;
    return 0;
}

/* ===================== 事件处理 ===================== */

static void xhci_complete_td(struct xhci_ring *ring, struct xhci_td *td,
                             uint32_t idx, uint32_t residual, uint32_t code)
{
    struct usbh_urb *urb = td->urb;

    if (code == TRB_CODE_SUCCESS || code == TRB_CODE_SHORT_PACKET) {
        uint32_t want = urb->transfer_buffer_length;
        uint32_t got = want - residual;

        /* 控制传输短包: data 段事件先记账, status 段事件才是终态 */
        if (td->kind == 1U && idx != td->last_idx) {
            td->actual = got;
            td->short_seen = 1U;
            return;
        }
        if (td->short_seen != 0U) {
            got = td->actual; /* 终态保持 data 段记下的实际长度 */
        }
        /* CherryUSB 合同(与 vendored EHCI 端口一致): 控制传输的
         * actual_length 计入 setup 段 8 字节, usbh_control_transfer 里
         * 再减回去 */
        if (td->kind == 1U) {
            got += 8U;
        }
        td->actual = got;
        td->error = 0;
    } else if (code == TRB_CODE_STOPPED || code == TRB_CODE_STOPPED_INVAL ||
               code == TRB_CODE_STOPPED_SHORT ||
               code == TRB_CODE_CMD_RING_STOPPED ||
               code == TRB_CODE_CMD_ABORTED) {
        td->actual = 0;
        td->error = -USB_ERR_SHUTDOWN;
    } else if (code == TRB_CODE_STALL) {
        td->actual = 0;
        td->error = -USB_ERR_STALL;
    } else if (code == TRB_CODE_TRANSACTION_ERR) {
        td->actual = 0;
        td->error = -USB_ERR_NAK;
    } else {
        td->actual = 0;
        td->error = -USB_ERR_IO;
    }

    ring->dequeue = (td->last_idx + 1U) % ring->num_trbs;
    td->state = (td->error == 0) ? 1U : 2U;

    /* 终态镜像回 TD 首槽: ring_put 在每个 TRB 槽存的是独立副本, 同步等待
     * 侧只看首槽的那份 */
    {
        struct xhci_td *first = &ring->tds[td->first_idx];

        first->state = td->state;
        first->actual = td->actual;
        first->error = td->error;
    }

    if (urb->complete != NULL) {
        urb->actual_length = td->actual;
        urb->errorcode = td->error;
        urb->complete(urb->arg, (td->error == 0) ? (int)td->actual : td->error);
    }
    td->urb = NULL;
}

static void xhci_handle_transfer_event(struct xhci_hcd *hcd, struct xhci_trb *ev)
{
    uint32_t dw2 = ev->dw2;
    uint32_t dw3 = ev->dw3;
    uint8_t slot_id = TRB3_SLOT_ID_GET(dw3);
    uint8_t dci = TRB3_EP_ID_GET(dw3);
    uint32_t code = TRB2_CODE_GET(dw2);
    uint32_t residual = TRB2_REM_GET(dw2);
    uintptr_t trb_ptr = (uintptr_t)ev->dw0 | ((uintptr_t)ev->dw1 << 32);
    struct xhci_dev *dev = xhci_find_dev(hcd, slot_id);
    struct xhci_ring *ring;
    uint32_t idx;
    struct xhci_td *td;

    /* NetBSD: RING_UNDERRUN/OVERRUN 事件的 trb 指针无意义, 直接忽略 */
    if (code == TRB_CODE_RING_UNDERRUN || code == TRB_CODE_RING_OVERRUN) {
        return;
    }
    if (dev == NULL || dci == 0U || dci >= XHCI_MAX_DCI) {
        usbh_console_printf("[USBH] xfer evt (no dev) slot=%u dci=%u code=%u\r\n",
                            slot_id, dci, code);
        return;
    }
    ring = dev->ep_rings[dci];
    if (ring == NULL) {
        usbh_console_printf("[USBH] xfer evt (no ring) slot=%u dci=%u\r\n",
                            slot_id, dci);
        return;
    }
    if (trb_ptr < (uintptr_t)ring->trbs ||
        trb_ptr >= (uintptr_t)ring->trbs + ring->num_trbs * TRB_SIZE) {
        usbh_console_printf("[USBH] xfer evt (bad ptr) slot=%u dci=%u ptr=%p\r\n",
                            slot_id, dci, (void *)trb_ptr);
        return;
    }
    idx = (uint32_t)((trb_ptr - (uintptr_t)ring->trbs) / TRB_SIZE);
    td = &ring->tds[idx];
    if (td->urb == NULL) {
        return;
    }
    xhci_complete_td(ring, td, idx, residual, code);
}

static void xhci_event_process(struct xhci_hcd *hcd)
{
    if (hcd->evt_busy) {
        hcd->evt_pending = 1;
        return;
    }
    hcd->evt_busy = 1;
    do {
        hcd->evt_pending = 0;

        xhci_dcache_invalidate(hcd->event_trbs, XHCI_EVENT_RING_SIZE * TRB_SIZE);

        while (1) {
            struct xhci_trb *ev = &hcd->event_trbs[hcd->event_ep];
            uint32_t type;

            if ((ev->dw3 & TRB3_CYCLE) != (uint32_t)hcd->event_cs) {
                break; /* 无新事件 */
            }
            type = TRB3_TYPE_GET(ev->dw3);

            switch (type) {
            case TRB_TYPE_TRANSFER:
                xhci_handle_transfer_event(hcd, ev);
                break;
            case TRB_TYPE_CMD_COMPLETE:
                /* 按 TRB 物理地址配对(NetBSD sc_command_addr 语义) */
                if ((uint32_t)(uintptr_t)ev->dw0 == hcd->cmd_trb_addr) {
                    hcd->cmd_code = TRB2_CODE_GET(ev->dw2);
                    hcd->cmd_slot_id = (uint8_t)TRB3_SLOT_ID_GET(ev->dw3);
                    hcd->cmd_done = 1;
                }
                break;
            case TRB_TYPE_PORT_STATUS: {
                uint8_t port = (uint8_t)((ev->dw0 >> 24) & 0xFFU);

                g_xhci_port_evt_seq++;
                if (port >= 1U && port <= hcd->num_ports && hcd->bus != NULL) {
                    hcd->bus->hcd.roothub_intbuf[0] |= (uint8_t)(1U << port);
                    /* ISR 内不做 mq 唤醒(SMP 端口 FromISR 路径未验证): 只置
                     * intbuf 与事件序号, 唤醒由适配层看门狗在任务侧完成。 */
                }
                break;
            }
            default:
                break;
            }

            hcd->event_ep = (hcd->event_ep + 1U) % XHCI_EVENT_RING_SIZE;
            if (hcd->event_ep == 0U) {
                hcd->event_cs ^= 1U;
            }
        }

        /* ERDP 前移并带 EHB(NetBSD softintr: 一次写完, 无独立清 EHB 步) */
        {
            uintptr_t erdp = (uintptr_t)&hcd->event_trbs[hcd->event_ep];

            xhci_w32_rts(hcd, XHCI_ERDP_LO(0),
                         ((uint32_t)erdp & ~0xFU) | XHCI_ERDP_BUSY);
            xhci_w32_rts(hcd, XHCI_ERDP_HI(0), (uint32_t)(erdp >> 32));
        }
    } while (hcd->evt_pending);

    hcd->evt_busy = 0;
}

/* ===================== Root Hub ===================== */

static void xhci_port_status(struct xhci_hcd *hcd, uint32_t port,
                             uint16_t *status, uint16_t *change)
{
    uint32_t ps = xhci_portsc(hcd, port);
    uint16_t st = 0;
    uint16_t ch = 0;
    uint32_t speed;

    if ((ps & XHCI_PS_CCS) != 0U) {
        st |= 0x0001U; /* CONNECTION */
    }
    if ((ps & XHCI_PS_PED) != 0U) {
        st |= 0x0002U; /* ENABLE */
    }
    if (XHCI_PS_PLS_GET(ps) == 0x3U) {
        st |= 0x0004U; /* SUSPEND */
    }
    if ((ps & XHCI_PS_OCA) != 0U) {
        st |= 0x0008U; /* OVERCURRENT */
    }
    if ((ps & XHCI_PS_PR) != 0U) {
        st |= 0x0010U; /* RESET */
    }
    if ((ps & XHCI_PS_PP) != 0U) {
        st |= 0x0100U; /* POWER */
    }
    speed = XHCI_PS_SPEED_GET(ps);
    if (speed == XHCI_PS_SPEED_HIGH) {
        st |= 0x0400U; /* HIGH_SPEED */
    } else if (speed == XHCI_PS_SPEED_LOW) {
        st |= 0x0200U; /* LOW_SPEED */
    }

    if ((ps & XHCI_PS_CSC) != 0U) {
        ch |= 0x0001U;
    }
    if ((ps & XHCI_PS_PEC) != 0U) {
        ch |= 0x0002U;
    }
    if ((ps & XHCI_PS_PLC) != 0U) {
        ch |= 0x0004U;
    }
    if ((ps & XHCI_PS_OCC) != 0U) {
        ch |= 0x0008U;
    }
    if ((ps & XHCI_PS_PRC) != 0U) {
        ch |= 0x0010U;
    }
    if ((ps & XHCI_PS_WRC) != 0U) {
        ch |= 0x8000U; /* C_BH_RESET */
    }

    *status = st;
    *change = ch;
}

int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup,
                         uint8_t *buf)
{
    struct xhci_hcd *hcd = &g_xhci[bus->busid];
    uint32_t port = setup->wIndex;
    uint32_t ps;
    int ret = 0;

    if (!hcd->running) {
        return -USB_ERR_SHUTDOWN;
    }
    if (port < 1U || port > hcd->num_ports) {
        return -USB_ERR_INVAL;
    }

    switch (setup->bRequest) {
    case HUB_REQUEST_GET_STATUS: {
        uint16_t st, ch;

        xhci_port_status(hcd, port, &st, &ch);
        if (buf != NULL) {
            buf[0] = (uint8_t)st;
            buf[1] = (uint8_t)(st >> 8);
            buf[2] = (uint8_t)ch;
            buf[3] = (uint8_t)(ch >> 8);
        }
        ret = 0;
        break;
    }
    case HUB_REQUEST_SET_FEATURE:
        switch (setup->wValue) {
        case HUB_PORT_FEATURE_POWER:
            ps = xhci_portsc(hcd, port);
            xhci_portsc_w(hcd, port, ps | XHCI_PS_PP);
            break;
        case HUB_PORT_FEATURE_RESET:
            /* 端口复位: 写 PR 后轮询 PRC。DWC3 USB2 端口在 PHY 链路未就绪
             * 时可能延迟完成, 超时 1s 报错不挂死。 */
            ps = xhci_portsc(hcd, port);
            xhci_portsc_w(hcd, port, (ps & ~XHCI_PS_PED) | XHCI_PS_PR);
            {
                uint32_t tmo;

                for (tmo = 0U; tmo < 1000U; tmo++) {
                    if ((xhci_portsc(hcd, port) & XHCI_PS_PRC) != 0U) {
                        break;
                    }
                    usb_osal_msleep(1);
                }
                if (tmo >= 1000U) {
                    USB_LOG_ERR("port %u reset timeout\r\n", port);
                }
            }
            break;
        case HUB_PORT_FEATURE_SUSPEND:
            ps = xhci_portsc(hcd, port);
            xhci_portsc_w(hcd, port, (ps & ~(0xFU << 5)) | XHCI_PS_PLS_SET(0x3) | XHCI_PS_LWS);
            break;
        default:
            ret = -USB_ERR_NOTSUPP;
            break;
        }
        break;
    case HUB_REQUEST_CLEAR_FEATURE:
        /* PORTSC 是 W1C + RW 混合寄存器, 读-改-写会闯祸:
         *  - PED(bit1) 是 RW1CS —— 把快照里的 PED=1 写回会直接禁用端口
         *    (2026-09-20 板验: 端口掉到 PED=0/PLS=7);
         *  - PP(bit9) 是 RW, 写 0 会关端口电源;
         *  - PLS 只在 LWS=1 时可写。
         * 因此只写目标 W1C 位 + 保留 PP(NetBSD ClearFeature 同款形状)。 */
        ps = XHCI_PS_PP;
        switch (setup->wValue) {
        case HUB_PORT_FEATURE_ENABLE:
            xhci_portsc_w(hcd, port, ps & ~XHCI_PS_PED);
            break;
        case HUB_PORT_FEATURE_SUSPEND:
            xhci_portsc_w(hcd, port, (ps & ~(0xFU << 5)) | XHCI_PS_PLS_SET(0x0) | XHCI_PS_LWS);
            break;
        case HUB_PORT_FEATURE_C_CONNECTION:
            xhci_portsc_w(hcd, port, ps | XHCI_PS_CSC);
            break;
        case HUB_PORT_FEATURE_C_ENABLE:
            xhci_portsc_w(hcd, port, ps | XHCI_PS_PEC);
            break;
        case HUB_PORT_FEATURE_C_SUSPEND:
            xhci_portsc_w(hcd, port, ps | XHCI_PS_PLC);
            break;
        case HUB_PORT_FEATURE_C_OVER_CURREN:
            xhci_portsc_w(hcd, port, ps | XHCI_PS_OCC);
            break;
        case HUB_PORT_FEATURE_C_RESET:
            xhci_portsc_w(hcd, port, ps | XHCI_PS_PRC);
            break;
        case HUB_PORT_FEATURE_C_BH_RESET:
            xhci_portsc_w(hcd, port, ps | XHCI_PS_WRC);
            break;
        default:
            ret = -USB_ERR_NOTSUPP;
            break;
        }
        break;
    case HUB_REQUEST_SET_HUB_DEPTH:
        ret = 0;
        break;
    default:
        ret = -USB_ERR_NOTSUPP;
        break;
    }
    return ret;
}

/* ===================== 中断入口 ===================== */

void USBH_IRQHandler(uint8_t busid)
{
    struct xhci_hcd *hcd;
    uint32_t usbsts, iman;

    if (busid >= CONFIG_USBHOST_MAX_BUS) {
        return;
    }
    hcd = &g_xhci[busid];
    if (!hcd->running) {
        return;
    }

    /* NetBSD xhci_intr1: 无关位则直退; RW1C 清 EINT/PCD/HSE(剥离保留位),
     * EINT 必须先于 IMAN.IP 清(level 中断顺序) */
    usbsts = xhci_r32(hcd, XHCI_USBSTS);
    if ((usbsts & (XHCI_STS_HSE | XHCI_STS_EINT | XHCI_STS_PCD | XHCI_STS_HCE)) == 0U) {
        return;
    }
    xhci_w32(hcd, XHCI_USBSTS, usbsts & ~XHCI_STS_RSVDP0);
    iman = xhci_r32_rts(hcd, XHCI_IMAN(0));
    xhci_w32_rts(hcd, XHCI_IMAN(0), iman | XHCI_IMAN_INTR_PEND);

    xhci_event_process(hcd);
}
