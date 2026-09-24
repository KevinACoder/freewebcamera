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
 *     measured 2026-09-20);
 *   - per-EP urb gate (CherryUSB contract: one urb in hardware per
 *     endpoint, extra submits wait on an urb list and are armed on
 *     completion - the EHCI port's bulk gate is the same shape), plus the
 *     NetBSD xhci_pipe_restart recovery trio (Stop/Reset Endpoint + Set TR
 *     Dequeue) so ctrl timeout / STALL / kill_urb no longer leave dead TDs
 *     jamming the ring (2026-09-23: 8821CU "mac power on failed" cascade).
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

#ifdef CONFIG_USBHOST_MULTI_HCD
/* Multi-HCD build: rename this port's global entry points so it can link
 * alongside the EHCI port in one image; usbh_core.c dispatches per bus
 * through usbh_xhci_ops. The renames sit after the includes on purpose -
 * the declarations above then double as prototypes for the new names, and
 * usbh_core.h's "#ifdef USBH_IRQHandler #error" guard has already been
 * evaluated. Function bodies below are untouched. */
#define usb_hc_init            usbh_xhci_hc_init
#define usb_hc_deinit          usbh_xhci_hc_deinit
#define usbh_get_frame_number  usbh_xhci_get_frame_number
#define usbh_roothub_control   usbh_xhci_roothub_control
#define usbh_submit_urb        usbh_xhci_submit_urb
#define usbh_kill_urb          usbh_xhci_kill_urb
#define USBH_IRQHandler        usbh_xhci_irq

/* Prototypes for the renamed names: the declarations in usb_hc.h were
 * parsed under the old names before these macros existed, and the ops
 * table below needs the real types in scope. */
int usbh_xhci_hc_init(struct usbh_bus *bus);
int usbh_xhci_hc_deinit(struct usbh_bus *bus);
uint16_t usbh_xhci_get_frame_number(struct usbh_bus *bus);
int usbh_xhci_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf);
int usbh_xhci_submit_urb(struct usbh_urb *urb);
int usbh_xhci_kill_urb(struct usbh_urb *urb);
void usbh_xhci_irq(uint8_t busid);
#endif

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

/* EP 状态(NetBSD xhcireg.h XHCI_EPCTX_0_EPSTATE, EP ctx dword0 [2:0]) */
#define EP_CTX_0_EPSTATE_GET(x) ((x) & 0x7U)
#define XHCI_EPSTATE_HALTED     2U
#define XHCI_EPSTATE_STOPPED    3U

/* 传输事件取证(2026-09-23 rtw88/xHCI 定界轮): 1=非 SUCCESS 事件打点,
 * 2=ep0 全事件打点(定判"TD 是否真的被执行"), 定位后归 0 */
#ifndef XHCI_EVENT_DEBUG
#define XHCI_EVENT_DEBUG 1
#endif

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
    /* per-EP urb 闸门(Linux urb_list 语义): 每 EP 硬件 1 个在途(active),
     * 多余的提交挂 wait 链(经 urb->hcpriv 串单向链), 完成一个 pop 下一个。
     * EHCI 侧 bulk gate 同型。gate_lock = 本核中断屏蔽 + 原子自旋, 供 ISR
     * 与多核线程安全共读写字段。cmd_ring 不使用这些字段。 */
    struct usbh_urb *active;
    struct usbh_urb *wait_head;
    struct usbh_urb *wait_tail;
    struct usbh_urb *killing;  /* 取消中的 urb: 完成事件不再 giveback */
    volatile uint8_t gate_lock;
    volatile uint8_t halted;   /* STALL/BABBLE/超时后需 Reset/Stop EP + Set TR Deq */
    volatile uint8_t recovering; /* Stop/Reset EP + Set TR Deq 恢复窗口: 提交只排队不入环 */
    uint8_t avoid_wrap;        /* 1=数据环: 触及 LINK 槽返回 -2 走环复位;
                                * 0=命令环: 自然回卷(单 TRB 命令, LINK 恒落
                                * 在命令边界, 走 LINK 是标准形态; 2026-09-23
                                * 板验: 命令环误用 -2 策略会把回卷当 NOMEM,
                                * 恢复命令永久失败) */
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

/* 命令完成上下文: do_command 的等待者把栈上 ctx 挂到 hcd->cmd_waiter,
 * 事件消费者按 TRB 物理地址配对后回填。生命周期仅限 do_command 调用内:
 * 成功路径 done=1 是消费者对 ctx 的最后一笔写(RELEASE, code/slot_id 先于
 * 它), 等待者读到即可收栈; 超时路径必须先占事件环闸门再摘 cmd_waiter,
 * 防止消费者拿着旧指针写已失效的栈。 */
struct xhci_cmd_ctx {
    volatile uint32_t done;
    uint32_t code;
    uint8_t slot_id;
    uint32_t trb_addr;
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
    /* 命令环(一次一条, NetBSD sc_command_addr 语义)。完成状态经 cmd_waiter
     * 回填到发命令者的栈上上下文: 旧版全局 cmd_done/cmd_trb_addr 在多线程
     * 并发发命令(hub 枚举 / worker 看门狗 kill / taskq ctrl)时会互相作废
     * ——后写者抹掉先写者的配对地址, 先写者超时后的 CA+环复位又把后写者
     * 在飞命令一并作废。 */
    struct xhci_ring cmd_ring;
    volatile uint32_t cmd_busy;
    struct xhci_cmd_ctx *volatile cmd_waiter; /* 在飞命令的完成上下文 */
    volatile uint32_t in_event;  /* 事件环消费中: giveback 回调禁同步 submit/kill */
    uint32_t hse_count;          /* USBSTS.HSE 观测计数(打印限流) */
    /* DCBAA + slots */
    uint64_t *dcbaa;
    struct xhci_dev devs[XHCI_MAX_SLOTS];
    uint8_t port_slot[8];
    /* 端口事件序号: ISR 每吞掉一个 Port Status Change 事件加一。适配层的
     * 热插拔看门狗轮询它决定是否唤醒 hub 线程(vendored 的 roothub_intbuf
     * 永不清零, 缓冲内容区分不了"已处理"与"新事件", 序号可以)。
     * per-instance: 双 xHCI 的 ISR 各自递增各自实例的序号。 */
    volatile uint32_t port_evt_seq;
    uint32_t addr_prints;      /* BSR=0 分配地址打印限流计数 */
};

static struct xhci_hcd g_xhci[CONFIG_USBHOST_MAX_BUS];

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
    ring->active = NULL;
    ring->wait_head = NULL;
    ring->wait_tail = NULL;
    ring->killing = NULL;
    ring->gate_lock = 0U;
    ring->halted = 0U;
    ring->recovering = 0U;
    ring->avoid_wrap = 1U; /* 数据环默认; 命令环初始化后显式清 0 */
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
 * 找回)。返回首 TRB 索引;-2 = 批量会跨环尾(LINK 落进 TD 中间)。
 *
 * LINK 不许落进 TD 中间: NetBSD xhci.c 明言 "arbitrary aligned LINK trb
 * definitely fail"(Ivy Bridge 实证; ASMedia 类控制器在 LINK 后接不上目标
 * TRB 时直接锁死)。2026-09-23 板验: DWC3 上 control TD 恰跨 254/255/0 时
 * 五秒无任何事件, Stop EP 停在 idx=0 - 同症。跨回卷的批量必须先复位环
 * (见调用方)再从 0 重装。
 *
 * 策略(2026-09-23 第二次板验定案): 本驱动不让硬件自然回卷 —— 凡批量
 * 会触及 LINK 槽(跨 LINK 或恰从 LINK 起批)一律 -2 走环复位 + SET_TR_DEQ
 * 重挂(板验机制)。从 LINK 槽起批时旧循环让 LINK 写吃掉一个载荷迭代,
 * STATUS TRB 没上环: 设备收到 SETUP+DATA 后等 STATUS 永久挂起(8821CU
 * 固件下载 DA 写"设备失聪"真因)。循环仍保留正确的 LINK 写作为安全网。
 */
static int xhci_ring_put(struct xhci_ring *ring, const struct xhci_trb *trbs,
                         uint32_t ntrbs, struct xhci_td *td)
{
    uint32_t ri = ring->ep;
    uint8_t cs = ring->cs;
    uint32_t first_slot = 0xFFFFFFFFU;
    uint8_t first_cs = 0U;
    uint32_t i;

    /* 数据环: 批量会跨过 LINK 槽、或恰从 LINK 槽起批 → -2, 走环复位
     * 路径(板验过的机制), 硬件只按 SET_TR_DEQ 重挂的环跑, 不做自然回卷。
     * 2026-09-23 板验: SA 写 TD 恰在 252..254 收尾(ep=255), 下一批从
     * LINK 槽起批 —— 旧代码 LINK 写吃掉一个载荷迭代, STATUS TRB 没上环:
     * 设备收到 SETUP+DATA 后等 STATUS 永久挂起, 主机侧 5s 超时, Stop EP
     * 等 TD 退役跟着挂死, CA 退役不了, 命令单元整体死亡(固件下载 DA 写
     * "设备失聪"全部现象)。恰收尾在 254 的批不触发(LINK 不被动到)。
     * 命令环(avoid_wrap=0)不回避: 单 TRB 命令的 LINK 恒在命令边界。 */
    if (ring->avoid_wrap != 0U &&
        ((ring->ep < ring->num_trbs - 1U &&
          ring->ep + ntrbs > ring->num_trbs - 1U) ||
         ring->ep == ring->num_trbs - 1U)) {
        return -2;
    }
    if (xhci_ring_space(ring) < ntrbs + 1U) {
        return -1;
    }

    for (i = 0U; i < ntrbs;) {
        struct xhci_trb t = trbs[i];
        struct xhci_td td_store = *td;

        if (ri == ring->num_trbs - 1U) {
            /* LINK TRB: 环回环基址, TC 位翻转消费者 cycle。
             * 只让位不占批: LINK 写不入载荷迭代(否则丢最后一个 TRB) */
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
        i++;
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
                           uint32_t timeout_ms, struct xhci_cmd_ctx *ctx)
{
    struct xhci_td td;
    struct xhci_trb cmd = *trb;
    int idx;
    uint32_t t;

    /* 不变量哨兵: giveback 回调全部设计为非阻塞投递(shim ring-post /
     * hub 唤醒 / modeswitch 信号量)。谁在事件上下文里同步发命令, 完成事件
     * 就只能由同一 event_process 循环消费 —— 自死锁, 必须当场报出。 */
    if (hcd->in_event != 0U) {
        USB_LOG_ERR("xhci: command type=%u issued in event context!\r\n",
                    TRB3_TYPE_GET(cmd.dw3));
    }

    if (__atomic_test_and_set(&hcd->cmd_busy, __ATOMIC_ACQUIRE)) {
        return -USB_ERR_BUSY;
    }
    memset(ctx, 0, sizeof(*ctx));
    idx = xhci_ring_put(&hcd->cmd_ring, &cmd, 1U, &td);
    if (idx < 0) {
        __atomic_clear(&hcd->cmd_busy, __ATOMIC_RELEASE);
        return -USB_ERR_NOMEM;
    }
    ctx->trb_addr = (uint32_t)(uintptr_t)&hcd->cmd_ring.trbs[idx];
    __atomic_store_n(&hcd->cmd_waiter, ctx, __ATOMIC_RELEASE);
    xhci_dcache_clean(hcd->cmd_ring.trbs, hcd->cmd_ring.num_trbs * TRB_SIZE);
    *(volatile uint32_t *)hcd->db = 0U; /* doorbell 0, target 0 = 命令环 */

    t = 0U;
    while (t < timeout_ms) {
        xhci_event_process(hcd);
        if (__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE) != 0U) {
            break;
        }
        usb_osal_msleep(1);
        t++;
    }
    if (ctx->done == 0U) {
        /* 超时收尾: 先独占事件环闸门, 保证没有消费者还握着 ctx, 再复查
         * 一次(竞态完成)并摘 cmd_waiter, 之后栈上 ctx 才允许失效 */
        while (__atomic_exchange_n(&hcd->evt_busy, 1U, __ATOMIC_ACQ_REL) != 0U) {
            usb_osal_msleep(1);
        }
        if (__atomic_load_n(&ctx->done, __ATOMIC_ACQUIRE) == 0U) {
            hcd->cmd_waiter = NULL;
        }
        __atomic_store_n(&hcd->evt_busy, 0U, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&hcd->cmd_waiter, NULL, __ATOMIC_RELEASE);
        __atomic_clear(&hcd->cmd_busy, __ATOMIC_RELEASE);
    }
    if (ctx->done == 0U) {
        /* NetBSD xhci_abort_command: 写 CRCR.CA, 等 CRR 自清, 复位环。
         * 取证 dump: HSE/HCH/CNR 区分控制器停摆与软件漏事件; CA 写后
         * CRR 是否自清区分命令单元真停与完成事件丢失。
         * 注意全程仍持有 cmd_busy: 并发新命令若此刻入队, 会被 CA/环
         * 复位一并作废 —— 那正是本修复要消除的互相破坏。 */
        uint32_t usbsts = xhci_r32(hcd, XHCI_USBSTS);
        uint32_t crcr = xhci_r32(hcd, XHCI_CRCR_LO);
        uint32_t i;

        USB_LOG_ERR("xhci: cmd timeout type=%u slot=%u ep=%u "
                    "USBSTS=%08x%s%s%s%s USBCMD=%08x IMAN=%08x CRCR=%08x "
                    "swring(ep=%u cs=%u)\r\n",
                    TRB3_TYPE_GET(cmd.dw3), TRB3_SLOT_ID_GET(cmd.dw3),
                    TRB3_EP_ID_GET(cmd.dw3), usbsts,
                    (usbsts & XHCI_STS_HSE) != 0U ? " HSE" : "",
                    (usbsts & XHCI_STS_HCH) != 0U ? " HCH" : "",
                    (usbsts & XHCI_STS_CNR) != 0U ? " CNR" : "",
                    (usbsts & XHCI_STS_HCE) != 0U ? " HCE" : "",
                    xhci_r32(hcd, XHCI_USBCMD),
                    xhci_r32_rts(hcd, XHCI_IMAN(0)), crcr,
                    hcd->cmd_ring.ep, hcd->cmd_ring.cs);
        xhci_w32(hcd, XHCI_CRCR_LO, crcr | XHCI_CRCR_LO_CA);
        for (i = 0U; i < 500U; i++) {
            if ((xhci_r32(hcd, XHCI_CRCR_LO) & XHCI_CRCR_LO_CRR) == 0U) {
                break;
            }
            usb_osal_msleep(1);
        }
        if (i == 500U) {
            USB_LOG_ERR("xhci: CRCR.CA did not retire (CRR stuck), "
                        "command unit dead\r\n");
        }
        hcd->cmd_ring.ep = 0;
        hcd->cmd_ring.cs = 1;
        hcd->cmd_ring.dequeue = 0; /* 环整体重挂: 软件双指针一并对齐 */
        xhci_w32(hcd, XHCI_CRCR_LO,
                 (uint32_t)(uintptr_t)hcd->cmd_ring.trbs | XHCI_CRCR_LO_RCS);
        USB_LOG_ERR("command timeout (type=%u)\r\n", TRB3_TYPE_GET(cmd.dw3));
        __atomic_clear(&hcd->cmd_busy, __ATOMIC_RELEASE);
        return -USB_ERR_TIMEOUT;
    }
    if (ctx->code != TRB_CODE_SUCCESS) {
        return -USB_ERR_IO;
    }
    return 0;
}

static int xhci_cmd_enable_slot(struct xhci_hcd *hcd, uint8_t *slot_id)
{
    struct xhci_trb trb;
    struct xhci_cmd_ctx ctx;
    int ret;

    memset(&trb, 0, sizeof(trb));
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_ENABLE_SLOT);
    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
    if (ret != 0) {
        return ret;
    }
    *slot_id = ctx.slot_id;
    return 0;
}

static int xhci_cmd_disable_slot(struct xhci_hcd *hcd, uint8_t slot_id)
{
    struct xhci_trb trb;
    struct xhci_cmd_ctx ctx;

    memset(&trb, 0, sizeof(trb));
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_DISABLE_SLOT) | TRB3_SLOT_ID(slot_id);
    return xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
}

/* ---- 端点恢复命令三件套(NetBSD xhci_reset_endpoint / xhci_stop_endpoint_cmd
 * / xhci_set_dequeue; 2026-09-23 前本文件只有类型定义没有实现 - STALL/超时
 * 后端点 halt 无解, 是 8821CU 控制传输级联超时的直接推手) ----
 * (定义在后文"urb 闸门"一节, 此处前置声明) */
static void xhci_ring_reset(struct xhci_ring *ring);
static uint32_t xhci_ring_lock(struct xhci_ring *ring);
static void xhci_ring_unlock(struct xhci_ring *ring, uint32_t save);
static void xhci_giveback(struct usbh_urb *urb, int status, uint32_t actual);
static uint32_t xhci_get_epstate(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                 uint32_t dci);
static void xhci_gate_kick(struct xhci_hcd *hcd, struct xhci_dev *dev,
                           uint32_t dci);

static int xhci_cmd_reset_ep(struct xhci_hcd *hcd, struct xhci_dev *dev,
                             uint32_t dci)
{
    struct xhci_trb trb;
    struct xhci_cmd_ctx ctx;

    memset(&trb, 0, sizeof(trb));
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_RESET_EP) | TRB3_SLOT_ID(dev->slot_id) |
              TRB3_EP_ID(dci);
    return xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
}

static int xhci_cmd_stop_ep(struct xhci_hcd *hcd, struct xhci_dev *dev,
                            uint32_t dci)
{
    struct xhci_trb trb;
    struct xhci_cmd_ctx ctx;

    memset(&trb, 0, sizeof(trb));
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_STOP_EP) | TRB3_SLOT_ID(dev->slot_id) |
              TRB3_EP_ID(dci);
    return xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
}

/* Dequeue 指针指回环基址, DCS=新消费者 cycle(NetBSD xhci_set_dequeue:
 * trb_0 = ring[0] | DCS; 配合 xhci_ring_reset 的 ep=0/cs=1) */
static int xhci_cmd_set_tr_dequeue(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                   uint32_t dci, struct xhci_ring *ring)
{
    struct xhci_trb trb;
    struct xhci_cmd_ctx ctx;

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)ring->trbs | 1U; /* DCS=1 */
    trb.dw1 = (uint32_t)((uintptr_t)ring->trbs >> 32);
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_SET_TR_DEQUEUE) |
              TRB3_SLOT_ID(dev->slot_id) | TRB3_EP_ID(dci);
    return xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
}

/*
 * 端点恢复(NetBSD xhci_pipe_restart, xhci.c:2247 形状): 按 EP state 分支
 * RESET_EP(HALTED)/STOP_EP(其余), 排干在途事件后软件清环, 再无条件
 * SET_TR_DEQUEUE。线程上下文专用(走同步命令); 勿在持有 ring lock 时调用。
 * victim 非空 = 取消在途 urb(kill 路径): 先置 killing 标记让在途的
 * STOPPED/SUCCESS 事件不再 giveback, 由这里按 victim_error 恰好归还一次并
 * 重臂队列头。victim 为空 = 纯恢复(STALL 后/同步超时后清环), urb 的失败
 * 状态由调用方自己返回。
 */
static int xhci_pipe_recover(struct xhci_hcd *hcd, struct xhci_dev *dev,
                             uint32_t dci, struct xhci_ring *ring,
                             struct usbh_urb *victim, int victim_error)
{
    uint32_t state, save;
    uint32_t i;
    int ret;

    if (victim != NULL) {
        save = xhci_ring_lock(ring);
        if (ring->active == victim) {
            ring->active = NULL;
        }
        ring->killing = victim;
        ring->recovering = 1U; /* 恢复窗口内提交只入队, 由文末 kick 武装 */
        xhci_ring_unlock(ring, save);
    }
    {
        save = xhci_ring_lock(ring);
        ring->recovering = 1U;
        xhci_ring_unlock(ring, save);
    }

    state = xhci_get_epstate(hcd, dev, dci);
    if (state == XHCI_EPSTATE_HALTED) {
        ret = xhci_cmd_reset_ep(hcd, dev, dci);
    } else if (state != XHCI_EPSTATE_STOPPED) {
        ret = xhci_cmd_stop_ep(hcd, dev, dci);
    } else {
        ret = 0;
    }
    if (ret != 0) {
        USB_LOG_ERR("xhci: recover dci=%u state=%u stop/reset failed ret=%d\r\n",
                    dci, state, ret);
        /* 命令失败也继续: NetBSD xhci_abortx 同款, 软件侧照样清环重臂 */
    }

    /* 排干在途事件(Stop EP 的 STOPPED 传输事件在命令完成事件前后到达;
     * killing 已置位, 它们不会再触发 giveback) */
    for (i = 0U; i < 3U; i++) {
        xhci_event_process(hcd);
        usb_osal_msleep(1);
    }

    save = xhci_ring_lock(ring);
    xhci_ring_reset(ring);
    xhci_dcache_clean(ring->trbs, ring->num_trbs * TRB_SIZE);
    ring->halted = 0U;
    ring->recovering = 0U;
    xhci_ring_unlock(ring, save);

    ret = xhci_cmd_set_tr_dequeue(hcd, dev, dci, ring);
    if (ret != 0) {
        USB_LOG_ERR("xhci: recover dci=%u set tr dequeue failed ret=%d\r\n",
                    dci, ret);
        /* SET_TR_DEQ 失败 = 硬件 dequeue 没有重指: 此刻重装 TD 必然
         * 永不被取(2026-09-23 板验: 失败后照旧武装 → 5s 超时风暴)。
         * 把失败上抛, 调用方决定回错而不是重装。victim 仍要归还(kill
         * 路径的恰一次语义), 但不 kick 武装队头。 */
        if (victim != NULL) {
            xhci_giveback(victim, victim_error, 0);
        }
        return ret;
    }

    if (victim != NULL) {
        xhci_giveback(victim, victim_error, 0);
        xhci_gate_kick(hcd, dev, dci);
    }
    return 0;
}

/* 同步 bulk urb(timeout>0, 现役调用只有 modeswitch 的异步+回调形态之外的
 * 兜底路径)在锁外轮询事件等完成, 与 queue_control 同款纪律。 */
static int xhci_bulk_sync_wait(struct xhci_hcd *hcd, struct xhci_dev *dev,
                               uint32_t dci, struct usbh_urb *urb,
                               int first_idx)
{
    struct xhci_ring *ring = dev->ep_rings[dci];
    uint8_t *buf = urb->transfer_buffer;
    uint32_t len = urb->transfer_buffer_length;
    uint32_t dir_in = (urb->ep->bEndpointAddress & USB2_DIR_IN) != 0U;
    uint32_t tmo = 0U;
    int err;

    while (tmo < urb->timeout) {
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
        err = ring->tds[first_idx].error;
        if (err == -USB_ERR_STALL) {
            /* bulk 协议 stall: 先恢复环再返回, 下一次提交不必再踩 halted */
            (void)xhci_pipe_recover(hcd, dev, dci, ring, NULL, 0);
        }
        return err;
    }
    /* 超时: 死 TD 不留环(Stop EP + Set TR Dequeue 清环)后再报超时 */
    (void)xhci_pipe_recover(hcd, dev, dci, ring, NULL, 0);
    return -USB_ERR_TIMEOUT;
}

/* NetBSD xhci_host_dequeue: 重编程 ep 上下文前把传输环归零(TRB/td 全清,
 * ep=0, cs=1) - 否则已消费的 TRB 带着 cycle=1 在重臂的 dequeue 上"复活",
 * 控制器会重放旧 TD(2026-09-20: SET_ADDRESS Transaction Error 的根因)。
 * wait 链不清: 排队的 urb 属于上层 xfer, 归还走 abort/kill 路径。 */
static void xhci_ring_reset(struct xhci_ring *ring)
{
    memset(ring->trbs, 0, ring->num_trbs * TRB_SIZE);
    memset(ring->tds, 0, ring->num_trbs * sizeof(struct xhci_td));
    ring->ep = 0;
    ring->cs = 1;
    ring->dequeue = 0;
    ring->active = NULL;
    ring->killing = NULL;
}

/* ===================== urb 闸门 ===================== */

/* 本核中断屏蔽(防 ISR 抢自旋造成同核死锁) + 原子自旋(防它核线程)。临界区
 * 内只有内存写与门铃, 无阻塞调用, 自旋界有限。 */
static uint32_t xhci_ring_lock(struct xhci_ring *ring)
{
    uint32_t save = usb_osal_enter_critical_section();

    while (__atomic_test_and_set(&ring->gate_lock, __ATOMIC_ACQUIRE)) {
        /* 它核持锁: 自旋等待 */
    }
    return save;
}

static void xhci_ring_unlock(struct xhci_ring *ring, uint32_t save)
{
    __atomic_clear(&ring->gate_lock, __ATOMIC_RELEASE);
    usb_osal_leave_critical_section(save);
}

static void xhci_giveback(struct usbh_urb *urb, int status, uint32_t actual)
{
    urb->actual_length = actual;
    urb->errorcode = status;
    if (urb->complete != NULL) {
        urb->complete(urb->arg, status);
    }
}

/* NetBSD xhci_get_epstate: 从输出设备上下文读 EP 状态(先 invalidate) */
static uint32_t xhci_get_epstate(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                 uint32_t dci)
{
    (void)hcd;
    xhci_dcache_invalidate(dev->dev_ctx, dev->ctxsz * (1U + XHCI_MAX_DCI));
    return EP_CTX_0_EPSTATE_GET(xhci_out_ctx(dev, dci)[0]);
}

/*
 * bulk/intr: 单 NORMAL TD, 必须在 ring lock 内调用。>64KB 的 urb 按 64KB
 * 边界分段并 CHAIN 成一个 TD(事件语义与 control 相同: 非末块事件只记账,
 * 末块事件终态)。成功则 urb 成为本环在途(active)并敲门铃, 返回首 TRB
 * 下标; 失败返回负错误码, 环状态不变。
 */
static int xhci_arm_normal_locked(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                  uint32_t dci, struct usbh_urb *urb)
{
    struct xhci_ring *ring = dev->ep_rings[dci];
    struct xhci_td td;
    uint8_t *buf = urb->transfer_buffer;
    uint32_t len = urb->transfer_buffer_length;
    uint32_t dir_in = (urb->ep->bEndpointAddress & USB2_DIR_IN) != 0U;
    uint32_t ntrbs = 0U;
    int first_idx;

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
        if (first_idx == -2) {
            return -2; /* 跨回卷: 调用方复位环后重试(线程上下文) */
        }
        if (first_idx < 0) {
            return -USB_ERR_NOMEM;
        }
    }

    xhci_dcache_clean(ring->trbs, ring->num_trbs * TRB_SIZE);
    *(volatile uint32_t *)(hcd->db + (dev->slot_id << 2)) = dci;
    ring->active = urb;
    return first_idx;
}

/* 锁内: 从 wait 链取队头并武装。返回取下的 urb, *err<0 表示建环失败
 * (urb 已摘链, 调用方在锁外 giveback 后继续取下一条)。无可武装返回 NULL。 */
static struct usbh_urb *xhci_gate_pop_arm_locked(struct xhci_hcd *hcd,
                                                 struct xhci_dev *dev,
                                                 uint32_t dci,
                                                 struct xhci_ring *ring,
                                                 int *err)
{
    struct usbh_urb *u = ring->wait_head;
    int idx;

    *err = 0;
    if (u == NULL || ring->active != NULL || ring->halted != 0U ||
        ring->recovering != 0U) {
        return NULL;
    }
    ring->wait_head = (struct usbh_urb *)u->hcpriv;
    if (ring->wait_tail == u) {
        ring->wait_tail = NULL;
    }
    u->hcpriv = NULL;
    idx = xhci_arm_normal_locked(hcd, dev, dci, u);
    if (idx < 0) {
        /* -2(跨回卷) 在事件上下文无法跑恢复命令, 按 NOMEM 归还;
         * 驱动重提时走 submit 路径的复位重装 */
        *err = (idx == -2) ? -USB_ERR_NOMEM : idx;
    }
    return u;
}

/* 武装队列头(线程上下文用: kill/超时恢复清环后重臂)。建环失败连环归还。 */
static void xhci_gate_kick(struct xhci_hcd *hcd, struct xhci_dev *dev,
                           uint32_t dci)
{
    for (;;) {
        struct xhci_ring *ring = dev->ep_rings[dci];
        struct usbh_urb *u;
        int err = 0;
        uint32_t save;

        if (ring == NULL) {
            return;
        }
        save = xhci_ring_lock(ring);
        u = xhci_gate_pop_arm_locked(hcd, dev, dci, ring, &err);
        xhci_ring_unlock(ring, save);
        if (u == NULL || err == 0) {
            return;
        }
        xhci_giveback(u, err, 0);
    }
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
    struct xhci_cmd_ctx ctx;
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

    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
    if (bsr == 0U) {
        /* 校验输出上下文: 控制器应已把 slot 置 ADDRESSED 并回报分配地址 */
        uint32_t dw3, st, hw_addr;

        xhci_dcache_invalidate(dev->dev_ctx, dev->ctxsz * (1U + XHCI_MAX_DCI));
        dw3 = xhci_out_ctx(dev, 0)[3];
        st = (dw3 >> 27) & 0x1FU;
        hw_addr = dw3 & 0xFFU;
        /* 枚举风暴时这里会成千上万次连发, 每控制器只打前 8 次 */
        if (hcd->addr_prints < 8U) {
            usbh_console_printf("xhci: bus%u assigned usb addr %u (state=%u)\r\n",
                                hcd->bus->busid, hw_addr, st);
            hcd->addr_prints++;
        }
        if (st == SLOT_CTX_STATE_ADDRESSED && hw_addr != 0U) {
            ret = 0;
        } else if (ret == 0) {
            ret = -USB_ERR_IO;
        }
    }
    if (ret != 0) {
        USB_LOG_ERR("Address Device (bsr=%u addr=%u) failed, code=%u\r\n",
                    bsr, usb_addr, ctx.code);
        return ret;
    }
    dev->ep0_mps_hw = mps;
    return 0;
}

static int xhci_cmd_evaluate_context(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                     uint16_t mps)
{
    struct xhci_trb trb;
    struct xhci_cmd_ctx ctx;
    int ret;

    xhci_setup_ep0_input_ctx(dev, mps);

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)dev->input_ctx;
    trb.dw1 = 0;
    trb.dw2 = 0;
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_EVALUATE_CTX) | TRB3_SLOT_ID(dev->slot_id);

    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
    if (ret != 0) {
        USB_LOG_ERR("Evaluate Context failed, code=%u\r\n", ctx.code);
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
    struct xhci_cmd_ctx ctx;
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

    ret = xhci_do_command(hcd, &trb, CMD_TIMEOUT_MS, &ctx);
    if (ret != 0) {
        USB_LOG_ERR("Configure EP (dci=%u) failed, code=%u\r\n", dci,
                    ctx.code);
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
static void xhci_port_disconnect(struct xhci_hcd *hcd, uint8_t port);

static int xhci_setup_slot(struct xhci_hcd *hcd, struct usbh_hubport *hport)
{
    struct xhci_dev *dev;
    uint8_t slot_id = 0U;
    uint16_t mps = hport->ep0.wMaxPacketSize;
    int ret;

    if (hport->port < 1U || hport->port > hcd->num_ports) {
        return -USB_ERR_INVAL;
    }
    /* 该端口如有残留 slot(断开与重连竞态: 清 C_CONNECTION 时新设备已把
     * CCS 拉回 1, CLEAR 侧的检查看不到断开), 先释放再使能新 slot */
    xhci_port_disconnect(hcd, hport->port);
    ret = xhci_cmd_enable_slot(hcd, &slot_id);
    if (ret != 0) {
        USB_LOG_ERR("Enable slot failed, ret=%d\r\n", ret);
        return ret;
    }
    usbh_console_printf("xhci: xHCI slot %u enabled\r\n", slot_id);

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

/* 设备断开(hub 线程清 C_CONNECTION、端口已无设备时, 任务上下文): 释放该
 * 端口的 slot 并归还其内存。没有这一步, port_slot 留着死 slot——同端口
 * 重连设备的每笔控制传输都敲在旧传输环上, 门铃无人应答全部超时
 * (2026-09-23: modeswitch dongle 在 xHCI 上重连必现, EHCI 线无此问题)。 */
static void xhci_port_disconnect(struct xhci_hcd *hcd, uint8_t port)
{
    uint8_t slot_id = hcd->port_slot[port];
    struct xhci_dev *dev;
    uint32_t dci;

    if (slot_id == 0U) {
        return;
    }
    hcd->port_slot[port] = 0U;
    dev = xhci_find_dev(hcd, slot_id);
    if (dev == NULL) {
        return;
    }
    /* 归还本设备所有环上的在途/排队 urb(-SHUTDOWN, 恰好一次): 上层
     * abort_pipe/watchdog 不必再依赖这些 urb 的完成事件。Disable Slot
     * 命令之后事件环可能还有残余事件, 彼时 td->urb 已清, 自然丢弃。 */
    for (dci = 1U; dci < XHCI_MAX_DCI; dci++) {
        struct xhci_ring *ring = dev->ep_rings[dci];
        struct usbh_urb *u, *next;
        uint32_t save;

        if (ring == NULL) {
            continue;
        }
        save = xhci_ring_lock(ring);
        u = ring->active;
        ring->active = NULL;
        next = ring->wait_head;
        ring->wait_head = NULL;
        ring->wait_tail = NULL;
        ring->killing = NULL;
        ring->halted = 0U;
        xhci_ring_unlock(ring, save);
        if (u != NULL) {
            u->hcpriv = NULL;
            xhci_giveback(u, -USB_ERR_SHUTDOWN, 0);
        }
        while (next != NULL) {
            struct usbh_urb *nxt = (struct usbh_urb *)next->hcpriv;

            next->hcpriv = NULL;
            xhci_giveback(next, -USB_ERR_SHUTDOWN, 0);
            next = nxt;
        }
    }
    if (xhci_cmd_disable_slot(hcd, slot_id) != 0) {
        USB_LOG_ERR("xhci: disable slot %u failed\r\n", slot_id);
        /* 命令失败也继续回收软件状态: 硬件侧 slot 或许已随断电消失 */
    }
    hcd->dcbaa[slot_id] = 0U;
    xhci_dcache_clean(hcd->dcbaa, sizeof(uint64_t) * hcd->max_slots);
    for (dci = 1U; dci < XHCI_MAX_DCI; dci++) {
        if (dev->ep_rings[dci] != NULL) {
            usb_sys_mem_free(dev->ep_rings[dci]->trbs);
            usb_sys_mem_free(dev->ep_rings[dci]->tds);
            usb_sys_mem_free(dev->ep_rings[dci]);
            dev->ep_rings[dci] = NULL;
        }
    }
    usb_sys_mem_free(dev->input_ctx);
    dev->input_ctx = NULL;
    usb_sys_mem_free(dev->dev_ctx);
    dev->dev_ctx = NULL;
    dev->in_use = 0U;
    usbh_console_printf("xhci: slot %u disabled (port %u device gone)\r\n",
                        slot_id, port);
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
        usbh_console_printf("xhci: warning: HCIVERSION 0x%04x\r\n", hciver);
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
        "xhci: xHCI: %u ports, %u slots, ctxsz=%u pgsz=%u caplen=%u hcs1=0x%08x\r\n",
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
    /* 命令环自然回卷: 单 TRB 命令的 LINK 恒落命令边界, 不回避
     * (NetBSD sc_cmd_ring 同款; -2 策略仅用于数据环) */
    hcd->cmd_ring.avoid_wrap = 0U;

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
        usbh_console_printf("xhci: port %u PORTSC=0x%08x\r\n", i,
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
    usbh_console_printf("xhci: entry: cap[0]=0x%08x GSNPSID=0x%08x\r\n",
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
    usbh_console_printf("xhci: usb_hc_init done bus%u\r\n", busid);
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
    uint32_t save;
    int err;
    int ret;

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

    /* ep0 前一轮留下 halted(STALL/BABBLE): 先恢复环再入队, 否则本 TRB 也
     * 排在死 TD 后面(NetBSD 在 is_halted 时同样不敲 doorbell 先恢复) */
    if (ring->halted != 0U) {
        (void)xhci_pipe_recover(hcd, dev, 1U, ring, NULL, 0);
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

    save = xhci_ring_lock(ring);
    if (xhci_ring_space(ring) < ntrbs + 1U) {
        xhci_ring_unlock(ring, save);
        return -USB_ERR_NOMEM;
    }
    memset(&td, 0, sizeof(td));
    td.urb = urb;
    td.kind = 1U;
    td.first_idx = 0U;
    td.last_idx = ntrbs - 1U;
    first_idx = xhci_ring_put(ring, trbs, ntrbs, &td);
    if (first_idx == -2) {
        /* 触及 LINK 槽: 复位环回到 0 再重装一次(ep0 走线程上下文)。
         * 恢复失败(SET_TR_DEQ 没成)绝不能重装 —— 硬件 dequeue 没重指,
         * 装上的 TD 永不被取, 就是 5s 超时风暴的形状 */
        xhci_ring_unlock(ring, save);
        ret = xhci_pipe_recover(hcd, dev, 1U, ring, NULL, 0);
        if (ret != 0) {
            return ret;
        }
        save = xhci_ring_lock(ring);
        first_idx = xhci_ring_put(ring, trbs, ntrbs, &td);
    }
    if (first_idx < 0) {
        xhci_ring_unlock(ring, save);
        return -USB_ERR_NOMEM;
    }
    xhci_dcache_clean(ring->trbs, ring->num_trbs * TRB_SIZE);
    *(volatile uint32_t *)(hcd->db + (dev->slot_id << 2)) = 1U; /* target dci 1 */
    xhci_ring_unlock(ring, save);

#if XHCI_EVENT_DEBUG >= 2
    usbh_console_printf(
        "xhci: bus%u ep0 arm idx=%u type=%02x req=%02x val=%04x len=%04x\r\n",
        hcd->bus->busid, first_idx, setup->bmRequestType, setup->bRequest,
        setup->wValue, setup->wLength);
#endif

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
        err = ring->tds[first_idx].error;
        if (err == -USB_ERR_STALL) {
            /* ep0 协议 stall: 设备侧自愈, 主机侧立即恢复环 - 恢复用的
             * CLEAR_FEATURE 也要走 ep0, 不能把 halt 留到下一次提交 */
            (void)xhci_pipe_recover(hcd, dev, 1U, ring, NULL, 0);
        }
        return err;
    }
#if XHCI_EVENT_DEBUG
    usbh_console_printf(
        "xhci: bus%u ctrl timeout: type=%02x req=%02x val=%04x idx=%04x len=%04x\r\n",
        hcd->bus->busid, setup->bmRequestType, setup->bRequest, setup->wValue,
        setup->wIndex, setup->wLength);
#endif
    /* 超时: 死 TD 不留环 - 先标记取消(防迟到的完成事件回调), Stop EP +
     * Set TR Dequeue 清环后再返回。老实现把 TD 原地留在环里, 后续控制
     * 传输全部排在死 TD 后面永久超时(shim 三次重试堆 4 个死 TD), 即
     * 8821CU "mac power on failed" 的级联形态 */
    save = xhci_ring_lock(ring);
    ring->killing = urb;
    xhci_ring_unlock(ring, save);
    (void)xhci_pipe_recover(hcd, dev, 1U, ring, NULL, 0);
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
         * 判定必须带请求类型与 wLength: 标准 SET_ADDRESS =
         * bmRequestType 0x00(host-to-device/standard/device) + wLength 0;
         * RTL8821CU 的 vendor 寄存器访问 bRequest 恰好也是 5
         * (RTW_USB_CMD_REQ, bmRequestType 0x40/0xC0) - 只看 bRequest 会把
         * 驱动的每一笔寄存器读写都劫持成 Address Device, 写从未上线,
         * 读恒为 0(2026-09-23 板验: "mac power on failed" 真因)。
         * 本机 DWC3 的 BSR=0 是"控制器自动分配地址"语义(见
         * xhci_cmd_address_device), 分配值与 CherryUSB 簿记的 dev_addr
         * 无关 - 栈侧照常记 2, 线上地址以控制器为准。 */
        if (setup->bRequest == USB_REQUEST_SET_ADDRESS &&
            setup->bmRequestType == 0U && setup->wLength == 0U) {
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

    /* per-EP urb 闸门: 每 EP 硬件 1 个在途, 忙则挂 wait 链(urb->hcpriv
     * 串联), 完成路径 pop 下一个。上层(rtw88 RX x2 / TX 单 pipe 20+,
     * urtwn TX 8/pipe)按多在途语义提交, 与 EHCI bulk gate 行为对齐。 */
    {
        struct xhci_ring *ring = dev->ep_rings[dci];
        uint32_t save;

        if (ring->halted != 0U) {
            /* 上轮 STALL/BABBLE 留下的 halt: 提交线程上下文顺路恢复
             * (NetBSD 在 is_halted 时同样不敲 doorbell, 先 xhci_pipe_restart) */
            ret = xhci_pipe_recover(hcd, dev, dci, ring, NULL, 0);
            if (ret != 0) {
                return ret;
            }
        }

        for (;;) {
            save = xhci_ring_lock(ring);
            if (ring->active == NULL && ring->recovering == 0U) {
                ret = xhci_arm_normal_locked(hcd, dev, dci, urb);
                if (ret == -2) {
                    /* 触及 LINK 槽(NetBSD xhci.c: 跨 LINK TD 在 Ivy
                     * Bridge/ASMedia 类控制器实证锁死, 本 DWC3 板验同症):
                     * 复位环回到 0 再重装一次。恢复失败(SET_TR_DEQ 没
                     * 成)不许重装 —— 硬件 dequeue 没重指, TD 永不被取 */
                    xhci_ring_unlock(ring, save);
                    ret = xhci_pipe_recover(hcd, dev, dci, ring, NULL, 0);
                    if (ret != 0) {
                        return ret;
                    }
                    continue;
                }
                if (ret < 0) {
                    xhci_ring_unlock(ring, save);
                    return -USB_ERR_NOMEM;
                }
                xhci_ring_unlock(ring, save);
                if (urb->timeout == 0U) {
                    return 0; /* 异步: 完成由中断回调 */
                }
                return xhci_bulk_sync_wait(hcd, dev, dci, urb, ret);
            }
            /* 忙/恢复中: urb 挂 wait 链(hcpriv 串联), 完成路径武装 */
            if (urb->timeout != 0U) {
                /* 同步 urb 遇忙/恢复中 EP: 本栈同步 bulk 只剩兜底路径, 正常
                 * 不触达; 返回忙优于静默排队让同步者饿死(避免 EHCI 侧
                 * sync-park NULL 解引用同款场景) */
                xhci_ring_unlock(ring, save);
                return -USB_ERR_BUSY;
            }
            urb->hcpriv = NULL;
            if (ring->wait_tail != NULL) {
                ring->wait_tail->hcpriv = urb;
            } else {
                ring->wait_head = urb;
            }
            ring->wait_tail = urb;
            xhci_ring_unlock(ring, save);
            return 0; /* 排队: 完成路径武装 */
        }
    }
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    struct usbh_hubport *hport;
    struct xhci_hcd *hcd;
    struct xhci_dev *dev;
    struct xhci_ring *ring;
    struct usbh_urb *it, *prev;
    uint32_t dci, save;

    if (urb == NULL || urb->hport == NULL) {
        return -USB_ERR_INVAL;
    }
    hport = urb->hport;
    if (hport->bus == NULL || hport->bus->busid >= CONFIG_USBHOST_MAX_BUS) {
        return -USB_ERR_INVAL;
    }
    hcd = &g_xhci[hport->bus->busid];
    if (!hcd->running) {
        return 0;
    }
    dev = xhci_get_dev(hcd, hport);
    if (dev == NULL) {
        return 0; /* slot 已释放: 硬件上下文已随断开消失 */
    }
    if (urb->ep == &hport->ep0) {
        dci = 1U;
    } else {
        dci = xhci_dci_of(urb->ep);
    }
    if (dci < 1U || dci >= XHCI_MAX_DCI) {
        return -USB_ERR_INVAL;
    }
    ring = dev->ep_rings[dci];
    if (ring == NULL) {
        return 0;
    }

    save = xhci_ring_lock(ring);
    if (ring->killing == urb) {
        /* 已在取消流程中: 恰好一次归还由那次 kill 负责(modeswitch 超时后
         * 下一轮尝试会先 drop 残留完成量, 看门狗重复 kill 亦同) */
        xhci_ring_unlock(ring, save);
        return 0;
    }
    if (ring->active == urb) {
        ring->active = NULL;
        ring->killing = urb;
        xhci_ring_unlock(ring, save);
        /* 在途取消(NetBSD xhci_abortx): 按状态 Stop/Reset EP + Set TR
         * Dequeue 清环, urb 由恢复路径给回 -SHUTDOWN 并重臂队列头 */
        (void)xhci_pipe_recover(hcd, dev, dci, ring, urb, -USB_ERR_SHUTDOWN);
        return 0;
    }
    /* 排队中: 摘链后直接归还 */
    prev = NULL;
    for (it = ring->wait_head; it != NULL; prev = it,
         it = (struct usbh_urb *)it->hcpriv) {
        if (it == urb) {
            break;
        }
    }
    if (it != NULL) {
        if (prev == NULL) {
            ring->wait_head = (struct usbh_urb *)it->hcpriv;
        } else {
            prev->hcpriv = it->hcpriv;
        }
        if (ring->wait_tail == urb) {
            ring->wait_tail = prev;
        }
        it->hcpriv = NULL;
        xhci_ring_unlock(ring, save);
        xhci_giveback(urb, -USB_ERR_SHUTDOWN, 0);
        return 0;
    }
    xhci_ring_unlock(ring, save);
    return 0; /* 已完成/已归还 */
}

/* ===================== 事件处理 ===================== */

static void xhci_complete_td(struct xhci_hcd *hcd, struct xhci_dev *dev,
                             uint32_t dci, struct xhci_ring *ring,
                             struct xhci_td *td, uint32_t idx,
                             uint32_t residual, uint32_t code)
{
    struct usbh_urb *urb = td->urb;
    uint32_t want;
    uint32_t got;
    int error;
    int killed;
    uint32_t save;

    /* 迟到/重复事件守卫: Stop EP + Set TR Deq 恢复会让被停 TD 的每个
     * TRB 槽都可能收到一次终态事件(code=26/Stopped 类), 环复位后更是
     * 落在清零槽上。槽空(urb=NULL)或已完成(state!=0)一律丢弃 —— 否则
     * 同一 urb 被二次归还, shim worker 二次 SLIST_REMOVE 数据异常
     * (2026-09-24 板验 fatal, FAR=0xaa0100c0, 8821CU RX 64-TRB 聚合
     * TD 的其余槽副本未被清空正是复现条件)。 */
    if (urb == NULL || td->state != 0U) {
        return;
    }
    want = urb->transfer_buffer_length;

    if (code == TRB_CODE_SUCCESS || code == TRB_CODE_SHORT_PACKET) {
        got = want - residual;

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
        error = 0;
    } else if (code == TRB_CODE_STOPPED || code == TRB_CODE_STOPPED_INVAL ||
               code == TRB_CODE_STOPPED_SHORT ||
               code == TRB_CODE_CMD_RING_STOPPED ||
               code == TRB_CODE_CMD_ABORTED) {
        got = 0;
        error = -USB_ERR_SHUTDOWN;
    } else if (code == TRB_CODE_STALL || code == TRB_CODE_BABBLE) {
        got = 0;
        error = -USB_ERR_STALL;
    } else if (code == TRB_CODE_TRANSACTION_ERR) {
        got = 0;
        error = -USB_ERR_NAK;
    } else {
        got = 0;
        error = -USB_ERR_IO;
    }

    save = xhci_ring_lock(ring);
    ring->dequeue = (td->last_idx + 1U) % ring->num_trbs;
    td->actual = got;
    td->error = error;
    td->state = (error == 0) ? 1U : 2U;

    /* 终态镜像回 TD 首槽: ring_put 在每个 TRB 槽存的是独立副本, 同步等待
     * 侧只看首槽的那份 */
    {
        struct xhci_td *first = &ring->tds[td->first_idx];

        first->state = td->state;
        first->actual = td->actual;
        first->error = td->error;
    }

    if (ring->active == urb) {
        ring->active = NULL;
    }
    if (code == TRB_CODE_STALL || code == TRB_CODE_BABBLE) {
        /* 端点已 halt: 恢复(Reset/Stop EP + Set TR Deq)由下一次提交的
         * 线程上下文顺路执行(NetBSD xhci_pipe_restart_async 同款延后) */
        ring->halted = 1U;
    }
    killed = (ring->killing == urb);
    /* 整段清 urb: 多 TRB TD(RX 聚合 64 槽)每个槽都存有 urb 副本, 只清
     * 事件落点槽的话, 恢复路径的迟到事件落在其余槽就会拿旧副本二次
     * 归还(见函数头守卫注释)。state 保留: 同步等待靠首槽 state 判完成,
     * actual/error 已镜像 */
    {
        uint32_t i = td->first_idx;

        for (;;) {
            ring->tds[i].urb = NULL;
            if (i == td->last_idx) {
                break;
            }
            i = (i + 1U) % ring->num_trbs;
        }
    }
    xhci_ring_unlock(ring, save);

    if (!killed) {
        /* 异步 IN 完成的缓存维护: 设备 DMA 写完的缓冲必须先 invalidate
         * 再交给驱动读。同步等待路径(:683/:1710)各自做过, 异步(中断/
         * 轮询)完成路径此前完全缺失 —— CPU 读到分配期 memset 的零行,
         * demux 全是 pkt_len=0 的 "skipping short packet"(2026-09-24
         * 8821CU 信标全丢的直接原因) */
        if (got != 0U && (urb->ep->bEndpointAddress & 0x80U) != 0U) {
            xhci_dcache_invalidate(urb->transfer_buffer, got);
        }
        xhci_giveback(urb, (error == 0) ? (int)got : error, got);
        /* 武装队列头: 完成一个 pop 下一个(Linux urb_list 语义)。建环失败
         * 连环归还(环满/参数坏不该饿死后续 urb)。 */
        for (;;) {
            struct usbh_urb *nx;
            int err = 0;

            save = xhci_ring_lock(ring);
            nx = xhci_gate_pop_arm_locked(hcd, dev, dci, ring, &err);
            xhci_ring_unlock(ring, save);
            if (nx == NULL || err == 0) {
                break;
            }
            nx->hcpriv = NULL;
            xhci_giveback(nx, err, 0);
        }
    }
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
        usbh_console_printf("xhci: xfer evt (no dev) slot=%u dci=%u code=%u\r\n",
                            slot_id, dci, code);
        return;
    }
    ring = dev->ep_rings[dci];
    if (ring == NULL) {
        usbh_console_printf("xhci: xfer evt (no ring) slot=%u dci=%u\r\n",
                            slot_id, dci);
        return;
    }
    if (trb_ptr < (uintptr_t)ring->trbs ||
        trb_ptr >= (uintptr_t)ring->trbs + ring->num_trbs * TRB_SIZE) {
        usbh_console_printf("xhci: xfer evt (bad ptr) slot=%u dci=%u ptr=%p\r\n",
                            slot_id, dci, (void *)trb_ptr);
        return;
    }
    idx = (uint32_t)((trb_ptr - (uintptr_t)ring->trbs) / TRB_SIZE);
    td = &ring->tds[idx];
    if (td->urb == NULL) {
        return;
    }
#if XHCI_EVENT_DEBUG
    if (code != TRB_CODE_SUCCESS && code != TRB_CODE_SHORT_PACKET) {
        usbh_console_printf(
            "xhci: bus%u xfer evt slot=%u dci=%u idx=%u code=%u urb_state=%u\r\n",
            hcd->bus->busid, slot_id, dci, idx, code, td->state);
    }
#if XHCI_EVENT_DEBUG >= 2
    if (dci == 1U) {
        usbh_console_printf(
            "xhci: bus%u ep0 evt idx=%u code=%u urb=%p state=%u\r\n",
            hcd->bus->busid, idx, code, (void *)td->urb, td->state);
    }
#endif
#endif
    xhci_complete_td(hcd, dev, dci, ring, td, idx, residual, code);
}

static void xhci_event_process(struct xhci_hcd *hcd)
{
    /* 单消费者闸门: 原 evt_busy 读-改-写在 4 核 SMP 下两核可同时通过
     * (ISR 与轮询线程各在一条核上)。原子交换持锁, 输家只置 pending 便回
     * - IMAN PEND 未清, 中断会再来。 */
    if (__atomic_exchange_n(&hcd->evt_busy, 1U, __ATOMIC_ACQ_REL) != 0U) {
        hcd->evt_pending = 1;
        return;
    }
    hcd->in_event = 1U;
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
            case TRB_TYPE_CMD_COMPLETE: {
                /* 按 TRB 物理地址配对(NetBSD sc_command_addr 语义),
                 * 回填发命令者栈上上下文; cmd_waiter 已摘除的迟到完成
                 * (CA 中止/超时后)在此安全丢弃 */
                struct xhci_cmd_ctx *cw = hcd->cmd_waiter;

                /* 命令环软件 dequeue 推进: 一次只发一条命令, 每个完成
                 * 事件必退役恰好一槽。不推进则 space 核算把环当满,
                 * ~253 条命令后所有命令永远 -NOMEM(2026-09-23 板验:
                 * 扫描期 -2 恢复每信道烧一条命令, 645s 撞墙) */
                hcd->cmd_ring.dequeue =
                    (hcd->cmd_ring.dequeue + 1U) % hcd->cmd_ring.num_trbs;
                if (cw != NULL &&
                    (uint32_t)(uintptr_t)ev->dw0 == cw->trb_addr) {
                    cw->code = TRB2_CODE_GET(ev->dw2);
                    cw->slot_id = (uint8_t)TRB3_SLOT_ID_GET(ev->dw3);
                    __atomic_store_n(&cw->done, 1U, __ATOMIC_RELEASE);
                }
                break;
            }
            case TRB_TYPE_PORT_STATUS: {
                uint8_t port = (uint8_t)((ev->dw0 >> 24) & 0xFFU);

                hcd->port_evt_seq++;
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

    hcd->in_event = 0U;
    __atomic_store_n(&hcd->evt_busy, 0U, __ATOMIC_RELEASE);
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
            /* CSC 被清且端口已无设备 = 断开已确认: 释放该端口的 slot */
            if ((xhci_portsc(hcd, port) & XHCI_PS_CCS) == 0U) {
                xhci_port_disconnect(hcd, port);
            }
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
    if ((usbsts & XHCI_STS_HSE) != 0U) {
        /* 旧版静默 RW1C 掩盖控制器级错误: HSE 之后命令/事件机器可能
         * 整体停摆, 必须 visible(固件下载期设备死亡窗的取证位) */
        hcd->hse_count++;
        if (hcd->hse_count <= 8U || (hcd->hse_count & 0x3FU) == 0U) {
            USB_LOG_ERR("xhci: USBSTS Host System Error (count=%u)\r\n",
                        hcd->hse_count);
        }
    }
    xhci_w32(hcd, XHCI_USBSTS, usbsts & ~XHCI_STS_RSVDP0);
    iman = xhci_r32_rts(hcd, XHCI_IMAN(0));
    xhci_w32_rts(hcd, XHCI_IMAN(0), iman | XHCI_IMAN_INTR_PEND);

    xhci_event_process(hcd);
}

#ifdef CONFIG_USBHOST_MULTI_HCD
uint32_t usbh_xhci_port_evt_seq(uint8_t busid)
{
	if (busid >= CONFIG_USBHOST_MAX_BUS) {
		return 0U;
	}
	return g_xhci[busid].port_evt_seq;
}

void usbh_xhci_port_release(uint8_t busid, uint8_t port)
{
	struct xhci_hcd *hcd;

	if (busid >= CONFIG_USBHOST_MAX_BUS || port == 0U ||
	    port > g_xhci[busid].num_ports) {
		return;
	}
	hcd = &g_xhci[busid];
	if (!hcd->running) {
		return;
	}
	xhci_port_disconnect(hcd, port);
}

const struct usbh_hcd_ops usbh_xhci_ops = {
    .driver_name = "xhci",
    .hc_init = usbh_xhci_hc_init,
    .hc_deinit = usbh_xhci_hc_deinit,
    .get_frame_number = usbh_xhci_get_frame_number,
    .roothub_control = usbh_xhci_roothub_control,
    .submit_urb = usbh_xhci_submit_urb,
    .kill_urb = usbh_xhci_kill_urb,
    .irq = usbh_xhci_irq,
};
#endif
