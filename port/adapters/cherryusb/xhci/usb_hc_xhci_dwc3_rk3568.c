/*
 * @file   usb_hc_xhci_dwc3_rk3568.c
 * @brief  DWC3 xHCI host controller driver (CherryUSB usb_hc.h contract) for
 *         the rk3568 usbhost_dwc3 @ 0xFD000000 (xHCI regs at +0).
 *
 * Transplanted whole-file from the lab line (rk3568_lab/os/standalone,
 * commit cc3acfd6, Apache-2.0, same author) per decision D20: the register
 * sequences carry the u-boot `usb start` measurements and the bare-metal
 * failure post-mortems (Address Device Parameter Error 17, port reset stuck
 * in Polling, PP writes swallowed before the PHY settles) - sequence
 * fidelity beats restyling. Mechanical adaptations only: printf ->
 * usbh_console_printf (this image has no libc), the two missing prototypes
 * below, and nothing else.
 *
 * The original was written against: xHCI 1.1 spec (registers/TRB/context
 * layout), FreeBSD sys/dev/usb/controller/xhci.c (BSD-2-Clause), u-boot
 * drivers/usb/host/xhci.c + xhci-ring.c (TRB layout cross-check), u-boot
 * drivers/usb/dwc3 + FreeBSD dwc3.c (host-mode core init).
 *
 * HS/FS/LS (USB2 only - the SS lane's combphy serves SATA). URBs run
 * synchronous (poll-wait) and async (interrupt callback); cache discipline:
 * event ring invalidate, TRB/context/data flush via the glue's usb_dcache_*.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */
#include "usbh_core.h"
#include "usb_hc_xhci.h"
#include "usb_hub.h"
#include "usbh_hub.h"

#include <string.h>

/* Glue-provided platform hooks (B's usb_hc.h carries no prototypes for
 * these; the EHCI driver declares them weak in its own file). */
extern void usb_hc_low_level_init(struct usbh_bus *bus);
extern void usb_hc_low_level_deinit(struct usbh_bus *bus);
extern void *usb_sys_malloc_align(size_t align, size_t size);
extern unsigned long usb_hc_get_register_base(uint32_t id);
extern void usb_hc_enable_interrupt(uint32_t id);
extern void usb_hc_disable_interrupt(uint32_t id);

/* ===================== 常量 ===================== */
#define XHCI_MAX_SLOTS       64U
#define XHCI_MAX_PORTS       8U
#define XHCI_EVENT_RING_SIZE CONFIG_USBHOST_XHCI_EVENT_RING_SIZE
#define XHCI_CMD_RING_SIZE   CONFIG_USBHOST_XHCI_COMMAND_RING_SIZE
#define XHCI_TRANSFER_RING_SIZE CONFIG_USBHOST_XHCI_TRANSFER_RING_SIZE

/* 完成码(xHCI 1.1 §6.4.5) */
#define XHCI_CODE_SUCCESS        1U
#define XHCI_CODE_DATA_BUF_ERR   2U
#define XHCI_CODE_BABBLE         3U
#define XHCI_CODE_TRANSACTION_ERR 4U
#define XHCI_CODE_TRB_ERR        5U
#define XHCI_CODE_STALL          6U
#define XHCI_CODE_SHORT_PACKET   13U
#define XHCI_CODE_CMD_STOPPED    24U
#define XHCI_CODE_CMD_ABORTED    25U
#define XHCI_CODE_STOPPED        26U
#define XHCI_CODE_STOPPED_INVAL  27U

/* TRB dw3 位 */
#define TRB3_CYCLE        (1U << 0)
#define TRB3_ENT          (1U << 1) /* link TRB 的 TC 位 */
#define TRB3_ISP          (1U << 2)
#define TRB3_CHAIN        (1U << 4)
#define TRB3_IOC          (1U << 5)
#define TRB3_IDT          (1U << 6)
#define TRB3_BSR          (1U << 9)
#define TRB3_DCEP         (1U << 9)
#define TRB3_TRT_NONE     (0U << 16)
#define TRB3_TRT_OUT      (2U << 16)
#define TRB3_TRT_IN       (3U << 16)
#define TRB3_TYPE_SET(x)  (((x) & 0x3FU) << 10)
#define TRB3_TYPE_GET(x)  (((x) >> 10) & 0x3FU)
#define TRB3_SLOT_ID_GET(x) (((x) >> 24) & 0xFFU)
#define TRB3_EP_ID_GET(x)   (((x) >> 16) & 0x1FU)
#define TRB3_SLOT_ID(x)   (((x) & 0xFFU) << 24)
#define TRB3_EP_ID(x)     (((x) & 0x1FU) << 16)

/* TRB dw2 位 */
#define TRB2_LEN(x)       ((x) & 0x1FFFFU)
#define TRB2_TDSZ(x)      (((x) & 0x1FU) << 17)
#define TRB2_IRQ(x)       (((x) & 0x3FFU) << 22)
#define TRB2_REM_GET(x)   ((x) & 0xFFFFFFU)
#define TRB2_CODE_GET(x)  (((x) >> 24) & 0xFFU)

/* 上下文 */
#define SLOT_DEV_INFO(x)      (SLOT_CTX_DEV_INFO_ROUTE(0) | SLOT_CTX_DEV_INFO_PORT(x) | SLOT_CTX_DEV_INFO_CONTEXT(1))
#define SLOT_DEV_STATE(x)     (SLOT_CTX_DEV_STATE_ADDR(x) | SLOT_CTX_DEV_STATE_SLOT_STATE(0))
#define EP0_CTX_MPS(x)        (EP_CTX_0_MAX_PACKET_SET(x) | EP_CTX_0_INTERVAL_SET(0))
#define EP0_CTX_INFO          (EP_CTX_1_CERR_SET(3) | EP_CTX_1_EP_TYPE_SET(EP_TYPE_CONTROL))

#define TRB_MAX_BUFF_SIZE     0x10000U /* 64KB, TRB 缓冲不能跨 64KB 边界 */

#define USB2_DIR_IN           0x80U
#define USB2_EP_NUM_MASK      0x0FU

#define CMD_TIMEOUT_MS        500U
#define CTRL_TIMEOUT_MS       500U

/* xHCI 上下文布局(单位 u32)。实测 rk3568 DWC3 HCCPARAMS1 CSZ=1 ->
 * 上下文 64 字节(16 u32)。对齐 u-boot xhci-mem.c xhci_alloc_container_ctx:
 * INPUT 上下文中 ICC 也占一个 CTX_SIZE 槽。
 * 布局: ICC[16] + slot[16] + ep0[16] + epN[16] (CSZ=1)
 *      ICC 占 64B(只用前 32B), slot @ 64B, ep0 @ 128B */
#define XHCI_ICC_WORDS   16U
#define XHCI_CTX_WORDS   16U
#define XHCI_ICC_SLOT_OFF   XHCI_ICC_WORDS
#define XHCI_ICC_EP0_OFF    (XHCI_ICC_WORDS + XHCI_CTX_WORDS)
#define XHCI_ICC_EPN_OFF(n) (XHCI_ICC_WORDS + XHCI_CTX_WORDS + XHCI_CTX_WORDS * (n))
/* 对齐 u-boot xhci_alloc_container_ctx: INPUT = (MAX_EP_CTX_NUM+2) 个上下文槽,
 * DEVICE = (MAX_EP_CTX_NUM+1) 个。DWC3 可能按最大槽数读取上下文(2026-08-12
 * 实测: 只分配 9 槽时 Address Device 报 Parameter Error 17)。 */
#define XHCI_MAX_EP_CTX_NUM   31U
#define XHCI_INPUT_CTX_WORDS  ((XHCI_MAX_EP_CTX_NUM + 2U) * XHCI_CTX_WORDS)
#define XHCI_DEV_CTX_WORDS    ((XHCI_MAX_EP_CTX_NUM + 1U) * XHCI_CTX_WORDS)

/* ===================== 内部结构 ===================== */

struct xhci_td {
    struct usbh_urb *urb;
    volatile uint32_t state;   /* 0=pending 1=done ok 2=done err */
    uint32_t actual;
    int error;
};

struct xhci_ring {
    struct xhci_trb *trbs;
    uint32_t num_trbs;
    uint32_t enqueue;
    uint32_t dequeue;
    uint8_t cycle;
    struct xhci_td *tds;       /* [num_trbs], 只使用 TD 首 TRB 槽 */
};

struct xhci_dev {
    uint8_t in_use;
    uint8_t slot_id;
    struct usbh_hubport *hport;
    uint8_t ep0_maxpacket;     /* 硬件当前 ep0 MPS */
    uint32_t ep_configured;    /* 已 Configure Endpoint 的 ep index 位图 */
    struct xhci_ring *ep_rings[31];
    uint32_t (*input_ctx)[8];  /* ICC(2) + slot(1) + 7 ep ctx, 64 字节对齐 */
    uint32_t (*dev_ctx)[8];    /* slot ctx + 8 ep ctx (DCBAA 指向), 64 字节对齐 */
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
    /* 事件环 */
    struct xhci_trb *event_trbs;
    struct xhci_erst_seg *erst;
    uint32_t event_idx;
    uint8_t event_cycle;
    volatile uint32_t evt_busy;
    volatile uint32_t evt_pending;
    /* 命令环 */
    struct xhci_ring cmd_ring;
    volatile uint32_t cmd_done;
    uint32_t cmd_code;
    uint8_t cmd_slot_id;
    /* DCBAA */
    uint64_t *dcbaa;
    /* slots */
    struct xhci_dev devs[XHCI_MAX_SLOTS];
    uint8_t port_slot[XHCI_MAX_PORTS];
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

/* DWC3 全局寄存器(capbase + 绝对偏移, 与 dwc3_host_core_check 同款) */
static inline uint32_t dwc3_greg_read(struct xhci_hcd *hcd, uint32_t off)
{
    return *(volatile uint32_t *)(hcd->capbase + off);
}

static inline void dwc3_greg_write(struct xhci_hcd *hcd, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(hcd->capbase + off) = val;
}

static inline void xhci_ring_db_cmd(struct xhci_hcd *hcd)
{
    *(volatile uint32_t *)(hcd->db + 0) = XHCI_DB_TARGET(0);
}

static inline void xhci_ring_db_ep(struct xhci_hcd *hcd, uint32_t slot, uint32_t ep)
{
    *(volatile uint32_t *)(hcd->db + (slot << 2)) = XHCI_DB_TARGET(ep);
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

/* ===================== 环管理 ===================== */

static void xhci_ring_init(struct xhci_ring *ring, uint32_t num_trbs)
{
    ring->trbs = usb_sys_malloc_align(64U, num_trbs * TRB_SIZE);
    ring->tds = usb_sys_malloc_align(64U, num_trbs * sizeof(struct xhci_td));
    memset(ring->trbs, 0, num_trbs * TRB_SIZE);
    memset(ring->tds, 0, num_trbs * sizeof(struct xhci_td));
    ring->num_trbs = num_trbs;
    ring->enqueue = 0;
    ring->dequeue = 0;
    ring->cycle = 1;
    /* 末项为 link TRB(初始 cycle=1, 与 ring->cycle 一致) */
    ring->trbs[num_trbs - 1].dw3 = TRB3_TYPE_SET(TRB_TYPE_LINK) | TRB3_CYCLE;
}

static uint32_t xhci_ring_space(struct xhci_ring *ring)
{
    uint32_t used = (ring->enqueue - ring->dequeue + ring->num_trbs) % ring->num_trbs;

    return ring->num_trbs - 2 - used;
}

/* 入队一个 TRB, 返回其索引, 环满返回 -1 */
static int xhci_ring_enqueue(struct xhci_ring *ring, struct xhci_trb *trb)
{
    uint32_t idx = ring->enqueue;

    if (xhci_ring_space(ring) < 1) {
        return -1;
    }
    if (idx == ring->num_trbs - 1) {
        /* 当前位置是 link TRB: 写 link(cycle=当前, TC=1), 再翻转 */
        ring->trbs[idx].dw0 = (uint32_t)(uintptr_t)ring->trbs;
        ring->trbs[idx].dw1 = 0;
        ring->trbs[idx].dw2 = 0;
        ring->trbs[idx].dw3 = TRB3_TYPE_SET(TRB_TYPE_LINK) | TRB3_ENT | ring->cycle;
        ring->cycle ^= 1;
        ring->enqueue = 0;
        idx = 0;
    }
    ring->trbs[idx] = *trb;
    ring->trbs[idx].dw3 |= ring->cycle;
    ring->enqueue = (idx + 1) % ring->num_trbs;
    return (int)idx;
}

/* 计算端点 index: OUT ep n -> n, IN ep n -> 0x10 | n */
static inline uint32_t xhci_ep_index(struct usb_endpoint_descriptor *ep)
{
    return (ep->bEndpointAddress & USB2_EP_NUM_MASK) |
           ((ep->bEndpointAddress & USB2_DIR_IN) ? 0x10U : 0U);
}

/* ===================== 事件处理(前置声明) ===================== */
static void xhci_event_process(struct xhci_hcd *hcd);

/* ===================== 命令 ===================== */

static int xhci_cmd_wait(struct xhci_hcd *hcd, uint32_t timeout_ms)
{
    uint32_t t = 0;

    while (t < timeout_ms) {
        xhci_event_process(hcd);
        if (hcd->cmd_done) {
            return 0;
        }
        usb_osal_msleep(1);
        t++;
    }
    return -USB_ERR_TIMEOUT;
}

static int xhci_cmd_enable_slot(struct xhci_hcd *hcd, uint8_t *slot_id)
{
    struct xhci_trb trb;
    int ret;

    memset(&trb, 0, sizeof(trb));
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_ENABLE_SLOT);
    hcd->cmd_done = 0;
    if (xhci_ring_enqueue(&hcd->cmd_ring, &trb) < 0) {
        return -USB_ERR_NOMEM;
    }
    xhci_dcache_clean(hcd->cmd_ring.trbs, hcd->cmd_ring.num_trbs * TRB_SIZE);
    xhci_ring_db_cmd(hcd);
    ret = xhci_cmd_wait(hcd, CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    if (hcd->cmd_code != XHCI_CODE_SUCCESS) {
        return -USB_ERR_IO;
    }
    *slot_id = hcd->cmd_slot_id;
    usbh_console_printf("[USBH] Enable Slot -> slot_id=%u\r\n", hcd->cmd_slot_id);
    return 0;
}

static int xhci_cmd_address_device(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                   uint8_t bsr, uint8_t usb_addr, uint8_t mps)
{
    struct xhci_trb trb;
    uint32_t *icc = dev->input_ctx[0];
    uint32_t *slot_ctx = dev->input_ctx[XHCI_ICC_SLOT_OFF];
    uint32_t *ep0_ctx = dev->input_ctx[XHCI_ICC_EP0_OFF];
    int ret;

    memset(icc, 0, sizeof(uint32_t) * XHCI_INPUT_CTX_WORDS);
    icc[0] = 0x3; /* Add: slot + ep0 */

    {
        uint32_t speed = SLOT_CTX_SPEED_HS;

        if (dev->hport->speed == USB_SPEED_LOW) {
            speed = SLOT_CTX_SPEED_LS;
        } else if (dev->hport->speed == USB_SPEED_FULL) {
            speed = SLOT_CTX_SPEED_FS;
        }
        slot_ctx[0] = SLOT_CTX_DEV_INFO_LAST_CTX(1) |
                      SLOT_CTX_DEV_INFO_SPEED(speed) |
                      SLOT_CTX_DEV_INFO_ROUTE(0);
        slot_ctx[1] = SLOT_CTX_DEV_INFO2_PORT(dev->hport->port) |
                      SLOT_CTX_DEV_INFO2_MAX_EXIT(0);
        slot_ctx[2] = 0;
        slot_ctx[3] = SLOT_CTX_DEV_STATE_ADDR(usb_addr);
    }
    ep0_ctx[0] = EP_CTX_0_INTERVAL_SET(0);
    ep0_ctx[1] = EP_CTX_1_EP_TYPE_SET(EP_TYPE_CONTROL) |
                 EP_CTX_1_CERR_SET(3) |
                 EP_CTX_1_MAX_PACKET_SET(mps);
    /* TR Dequeue Pointer 必须指向 EP0 传输环(规范 §4.6.5), 0 会 Parameter Error */
    ep0_ctx[2] = (uint32_t)(uintptr_t)dev->ep_rings[0]->trbs | EP_CTX_2_CYCLE;
    ep0_ctx[3] = EP_CTX_3_AVG_TRB_LEN(8);
    ep0_ctx[4] = 0;
    ep0_ctx[5] = 0;
    ep0_ctx[6] = 0;
    ep0_ctx[7] = 0;

    xhci_dcache_clean(dev->input_ctx, sizeof(uint32_t) * 10 * 8);

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)dev->input_ctx;
    trb.dw1 = 0;
    trb.dw2 = 0;
    usbh_console_printf("[USBH] ADDRDEV slot=%u bsr=%u icc@0x%08lx add=0x%08x\r\n",
           dev->slot_id, bsr, (unsigned long)dev->input_ctx, icc[0]);
    usbh_console_printf("[USBH]   slot: %08x %08x %08x %08x | ep0: %08x %08x %08x %08x\r\n",
           slot_ctx[0], slot_ctx[1], slot_ctx[2], slot_ctx[3],
           ep0_ctx[0], ep0_ctx[1], ep0_ctx[2], ep0_ctx[3]);

    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_ADDRESS_DEV) | TRB3_SLOT_ID(dev->slot_id) |
              (bsr ? TRB3_BSR : 0);

    hcd->cmd_done = 0;
    if (xhci_ring_enqueue(&hcd->cmd_ring, &trb) < 0) {
        return -USB_ERR_NOMEM;
    }
    xhci_dcache_clean(hcd->cmd_ring.trbs, hcd->cmd_ring.num_trbs * TRB_SIZE);
    xhci_ring_db_cmd(hcd);
    ret = xhci_cmd_wait(hcd, CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    if (hcd->cmd_code != XHCI_CODE_SUCCESS) {
        uint32_t i;
        uint32_t guctl1, guctl;

        USB_LOG_ERR("Address Device failed, code=%u\r\n", hcd->cmd_code);
        /* 诊断(2026-09-20): ivac 后回读 = DDR 真实内容。若全 0 则 flush
         * (cvac) 根本没把数据送到硬件可见处; 若在则参数真被硬件拒绝。 */
        xhci_dcache_invalidate(dev->input_ctx, sizeof(uint32_t) * 48);
        usbh_console_printf("[USBH] DDR icc[0]=%08x slot:", dev->input_ctx[0][0]);
        for (i = 0; i < 4; i++) {
            usbh_console_printf(" %08x", dev->input_ctx[XHCI_ICC_SLOT_OFF][i]);
        }
        usbh_console_printf(" ep0:");
        for (i = 0; i < 4; i++) {
            usbh_console_printf(" %08x", dev->input_ctx[XHCI_ICC_EP0_OFF][i]);
        }
        usbh_console_printf("\r\n");
        guctl1 = dwc3_greg_read(hcd, DWC3_GUCTL1);
        guctl = dwc3_greg_read(hcd, DWC3_GUCTL);
        usbh_console_printf("[USBH] GUCTL=0x%08x GUCTL1=0x%08x\r\n", guctl, guctl1);
        return -USB_ERR_IO;
    }
    dev->ep0_maxpacket = mps;
    return 0;
}

static int xhci_cmd_evaluate_context(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                     uint8_t mps)
{
    struct xhci_trb trb;
    uint32_t *icc = dev->input_ctx[0];
    uint32_t *slot_ctx = dev->input_ctx[XHCI_ICC_SLOT_OFF];
    uint32_t *ep0_ctx = dev->input_ctx[XHCI_ICC_EP0_OFF];
    int ret;

    memset(icc, 0, sizeof(uint32_t) * XHCI_INPUT_CTX_WORDS);
    icc[0] = 0x3;

    {
        uint32_t speed = SLOT_CTX_SPEED_HS;

        if (dev->hport->speed == USB_SPEED_LOW) {
            speed = SLOT_CTX_SPEED_LS;
        } else if (dev->hport->speed == USB_SPEED_FULL) {
            speed = SLOT_CTX_SPEED_FS;
        }
        slot_ctx[0] = SLOT_CTX_DEV_INFO_LAST_CTX(1) |
                      SLOT_CTX_DEV_INFO_SPEED(speed) |
                      SLOT_CTX_DEV_INFO_ROUTE(0);
        slot_ctx[1] = SLOT_CTX_DEV_INFO2_PORT(dev->hport->port) |
                      SLOT_CTX_DEV_INFO2_MAX_EXIT(0);
        slot_ctx[2] = 0;
        slot_ctx[3] = SLOT_CTX_DEV_STATE_ADDR(dev->hport->dev_addr);
    }
    ep0_ctx[0] = EP_CTX_0_INTERVAL_SET(0);
    ep0_ctx[1] = EP_CTX_1_EP_TYPE_SET(EP_TYPE_CONTROL) |
                 EP_CTX_1_CERR_SET(3) |
                 EP_CTX_1_MAX_PACKET_SET(mps);
    /* TR Dequeue Pointer 必须指向 EP0 传输环(规范 §4.6.5), 0 会 Parameter Error */
    ep0_ctx[2] = (uint32_t)(uintptr_t)dev->ep_rings[0]->trbs | EP_CTX_2_CYCLE;
    ep0_ctx[3] = EP_CTX_3_AVG_TRB_LEN(8);
    ep0_ctx[4] = 0;
    ep0_ctx[5] = 0;
    ep0_ctx[6] = 0;
    ep0_ctx[7] = 0;

    xhci_dcache_clean(dev->input_ctx, sizeof(uint32_t) * 10 * 8);

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)dev->input_ctx;
    trb.dw1 = 0;
    trb.dw2 = 0;
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_EVALUATE_CTX) | TRB3_SLOT_ID(dev->slot_id);

    hcd->cmd_done = 0;
    if (xhci_ring_enqueue(&hcd->cmd_ring, &trb) < 0) {
        return -USB_ERR_NOMEM;
    }
    xhci_dcache_clean(hcd->cmd_ring.trbs, hcd->cmd_ring.num_trbs * TRB_SIZE);
    xhci_ring_db_cmd(hcd);
    ret = xhci_cmd_wait(hcd, CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    if (hcd->cmd_code != XHCI_CODE_SUCCESS) {
        USB_LOG_ERR("Evaluate Context failed, code=%u\r\n", hcd->cmd_code);
        return -USB_ERR_IO;
    }
    dev->ep0_maxpacket = mps;
    return 0;
}

/* 配置非 ep0 端点(首次使用时) */
static int xhci_cmd_configure_ep(struct xhci_hcd *hcd, struct xhci_dev *dev,
                                 uint32_t ep_index, uint8_t ep_type,
                                 uint16_t mps, uint8_t interval, uint8_t burst,
                                 struct xhci_ring *ring)
{
    struct xhci_trb trb;
    uint32_t *icc = dev->input_ctx[0];
    uint32_t *slot_ctx = dev->input_ctx[XHCI_ICC_SLOT_OFF];
    uint32_t *ep_ctx = dev->input_ctx[XHCI_ICC_EPN_OFF(ep_index - 1)];
    int ret;

    if (ep_index > 7) {
        return -USB_ERR_INVAL; /* 超出 input ctx 预留(ep0 + 7) */
    }

    memset(icc, 0, sizeof(uint32_t) * XHCI_INPUT_CTX_WORDS);
    icc[0] = 0x1 | (1U << ep_index); /* Add: slot + ep */

    {
        uint32_t speed = SLOT_CTX_SPEED_HS;

        if (dev->hport->speed == USB_SPEED_LOW) {
            speed = SLOT_CTX_SPEED_LS;
        } else if (dev->hport->speed == USB_SPEED_FULL) {
            speed = SLOT_CTX_SPEED_FS;
        }
        slot_ctx[0] = SLOT_CTX_DEV_INFO_LAST_CTX(ep_index + 1) |
                      SLOT_CTX_DEV_INFO_SPEED(speed) |
                      SLOT_CTX_DEV_INFO_ROUTE(0);
        slot_ctx[1] = SLOT_CTX_DEV_INFO2_PORT(dev->hport->port) |
                      SLOT_CTX_DEV_INFO2_MAX_EXIT(0);
        slot_ctx[2] = 0;
        slot_ctx[3] = SLOT_CTX_DEV_STATE_ADDR(dev->hport->dev_addr) |
                      SLOT_CTX_DEV_STATE_SLOT_STATE(SLOT_CTX_STATE_CONFIGURED);
    }
    ep_ctx[0] = EP_CTX_0_INTERVAL_SET(interval);
    ep_ctx[1] = EP_CTX_1_EP_TYPE_SET(ep_type) |
                 EP_CTX_1_CERR_SET(3) |
                 EP_CTX_1_MAX_PACKET_SET(mps) |
                 EP_CTX_1_MAX_BURST_SET(burst);
    ep_ctx[2] = (uint32_t)(uintptr_t)ring->trbs | EP_CTX_2_CYCLE;
    ep_ctx[3] = EP_CTX_3_AVG_TRB_LEN(8);
    ep_ctx[4] = 0;
    ep_ctx[5] = 0;
    ep_ctx[6] = 0;
    ep_ctx[7] = 0;

    xhci_dcache_clean(dev->input_ctx, sizeof(uint32_t) * 10 * 8);

    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)(uintptr_t)dev->input_ctx;
    trb.dw1 = 0;
    trb.dw2 = 0;
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_CONFIGURE_EP) | TRB3_SLOT_ID(dev->slot_id);

    hcd->cmd_done = 0;
    if (xhci_ring_enqueue(&hcd->cmd_ring, &trb) < 0) {
        return -USB_ERR_NOMEM;
    }
    xhci_dcache_clean(hcd->cmd_ring.trbs, hcd->cmd_ring.num_trbs * TRB_SIZE);
    xhci_ring_db_cmd(hcd);
    ret = xhci_cmd_wait(hcd, CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    if (hcd->cmd_code != XHCI_CODE_SUCCESS) {
        USB_LOG_ERR("Configure EP failed, code=%u\r\n", hcd->cmd_code);
        return -USB_ERR_IO;
    }
    dev->ep_configured |= (1U << ep_index);
    return 0;
}

/* ===================== 事件处理 ===================== */

static struct xhci_dev *xhci_find_dev(struct xhci_hcd *hcd, uint8_t slot_id)
{
    if (slot_id == 0 || slot_id >= hcd->max_slots) {
        return NULL;
    }
    if (!hcd->devs[slot_id].in_use) {
        return NULL;
    }
    return &hcd->devs[slot_id];
}

static void xhci_handle_transfer_event(struct xhci_hcd *hcd, struct xhci_trb *ev)
{
    uint32_t dw2 = ev->dw2;
    uint32_t dw3 = ev->dw3;
    uint8_t slot_id = TRB3_SLOT_ID_GET(dw3);
    uint8_t ep_id = TRB3_EP_ID_GET(dw3);
    uint32_t code = TRB2_CODE_GET(dw2);
    uint32_t residual = TRB2_REM_GET(dw2);
    uintptr_t trb_ptr = (uintptr_t)ev->dw0 | ((uintptr_t)ev->dw1 << 32);
    struct xhci_dev *dev = xhci_find_dev(hcd, slot_id);
    struct xhci_ring *ring;
    uint32_t idx;
    struct xhci_td *td;

    if (dev == NULL || ep_id == 0 || ep_id >= 31) {
        return;
    }
    ring = dev->ep_rings[ep_id];
    if (ring == NULL) {
        return;
    }
    idx = (uint32_t)((trb_ptr - (uintptr_t)ring->trbs) / TRB_SIZE);
    if (idx >= ring->num_trbs) {
        return;
    }
    td = &ring->tds[idx];
    if (td->urb == NULL) {
        return;
    }

    if (code == XHCI_CODE_SUCCESS || code == XHCI_CODE_SHORT_PACKET) {
        td->actual = td->urb->transfer_buffer_length - residual;
        td->error = 0;
    } else if (code == XHCI_CODE_STOPPED || code == XHCI_CODE_STOPPED_INVAL) {
        td->actual = 0;
        td->error = -USB_ERR_SHUTDOWN;
    } else if (code == XHCI_CODE_TRANSACTION_ERR || code == XHCI_CODE_STALL) {
        td->actual = 0;
        td->error = -USB_ERR_NAK;
    } else {
        td->actual = 0;
        td->error = -USB_ERR_IO;
    }

    /* 推进 dequeue 到 TD 末尾(CHAIN 链) */
    {
        uint32_t i = idx;

        while (i != ring->enqueue) {
            uint32_t next = (i + 1) % ring->num_trbs;

            if (!(ring->trbs[i].dw3 & TRB3_CHAIN)) {
                break;
            }
            i = next;
        }
        ring->dequeue = (i + 1) % ring->num_trbs;
    }

    td->state = (td->error == 0) ? 1 : 2;

    if (td->urb->complete != NULL) {
        td->urb->actual_length = td->actual;
        td->urb->errorcode = td->error;
        td->urb->complete(td->urb->arg, (td->error == 0) ? (int)td->actual : td->error);
    }
    td->urb = NULL;
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
            struct xhci_trb *ev = &hcd->event_trbs[hcd->event_idx];
            uint32_t type;

            if (!(ev->dw3 & hcd->event_cycle)) {
                break; /* 无新事件 */
            }
            type = TRB3_TYPE_GET(ev->dw3);

            switch (type) {
            case TRB_TYPE_TRANSFER:
                xhci_handle_transfer_event(hcd, ev);
                break;
            case TRB_TYPE_CMD_COMPLETE:
                hcd->cmd_code = TRB2_CODE_GET(ev->dw2);
                hcd->cmd_slot_id = (uint8_t)TRB3_SLOT_ID_GET(ev->dw3);
                hcd->cmd_done = 1;
                usbh_console_printf("[USBH] CMDCOMPLETE code=%u slot=%u ev={0x%08x 0x%08x 0x%08x 0x%08x}\r\n",
                       hcd->cmd_code, hcd->cmd_slot_id,
                       ev->dw0, ev->dw1, ev->dw2, ev->dw3);
                break;
            case TRB_TYPE_PORT_STATUS: {
                uint8_t port = (uint8_t)((ev->dw2 >> 24) & 0xFFU);

                g_xhci_port_evt_seq++;
                usbh_console_printf("[USBH] Port Status Event port=%u PORTSC=0x%08x\r\n",
                                    port, xhci_portsc(hcd, port));
                if (port >= 1 && port <= hcd->num_ports && hcd->bus != NULL) {
                    hcd->bus->hcd.roothub_intbuf[0] |= (uint8_t)(1U << port);
                    /* ISR 内不做 mq 唤醒(SMP 端口 FromISR 路径未验证): 只置
                     * intbuf 与事件序号, 唤醒由适配层看门狗在任务侧完成。 */
                }
                break;
            }
            default:
                break;
            }

            hcd->event_idx = (hcd->event_idx + 1) % XHCI_EVENT_RING_SIZE;
            if (hcd->event_idx == 0) {
                hcd->event_cycle ^= 1;
            }
        }

        /* 更新 ERDP(清 BUSY) */
        {
            uintptr_t erdp = (uintptr_t)&hcd->event_trbs[hcd->event_idx];

            xhci_w32_rts(hcd, XHCI_ERDP_LO(0), (uint32_t)erdp & ~0x7U);
            xhci_w32_rts(hcd, XHCI_ERDP_HI(0), (uint32_t)(erdp >> 32));
        }
    } while (hcd->evt_pending);

    hcd->evt_busy = 0;
}

/* ===================== 控制器初始化 ===================== */

/* DWC3 核心态检查(只读, 零写入)。固件/OS 分工(D36, M3-A SATA 先例):
 * PHY、CRU/PMUCRU 门控、PD_PIPE 电源岛与 VBUS 由 U-Boot preboot 的
 * `usb start` 负责, OS 继承活控制器, 绝不重配 PHY。这里只验三件事:
 * GSNPSID 签名、PRTCAP=host、xHCI 孔径解码; 任一不满足 = preboot
 * 没跑或状态被破坏, 明确报错而不是带病硬闯。
 * 2026-09-19 板验教训: 本驱动+glue 自带的全套平台序列(CRU 复位脉冲、
 * GCTL CORESOFTRESET/PHYSOFTRST 八步、GUSB2PHYCFG 改写)会把活控制器
 * 打成"xHCI 孔径与参数寄存器读零" - 同序列在 u-boot 逐步重放全程无害,
 * 环境差异使裸机重配不可行, 故整体删除。 */
static int dwc3_host_core_check(struct xhci_hcd *hcd)
{
    volatile uint32_t *dwc3 = (volatile uint32_t *)hcd->capbase;
    uint32_t revision, gctl;

    revision = dwc3[DWC3_GSNPSID / 4];
    if ((revision & DWC3_GSNPSID_MASK) != DWC3_GSNPSID_VAL) {
        USB_LOG_ERR("DWC3 not found, GSNPSID=0x%08x\r\n", revision);
        return -USB_ERR_INVAL;
    }
    gctl = dwc3[DWC3_GCTL / 4];
    if (((gctl >> 12) & 0x3U) != DWC3_GCTL_PRTCAP_HOST) {
        USB_LOG_ERR("DWC3 not in host mode (GCTL=0x%08x) - preboot `usb start` missing\r\n", gctl);
        return -USB_ERR_INVAL;
    }
    if ((dwc3[0] & 0xFFU) == 0U) {
        USB_LOG_ERR("xHCI aperture dead (cap=0) - preboot `usb start` missing\r\n");
        return -USB_ERR_INVAL;
    }
    usbh_console_printf("[USBH] DWC3 inherited: GSNPSID=0x%08x GCTL=0x%08x GUSB2PHYCFG=0x%08x\r\n",
                        revision, gctl, dwc3[DWC3_GUSB2PHYCFG / 4]);
    return 0;
}

static int xhci_controller_init(struct xhci_hcd *hcd)
{
    uint32_t cap, hcs1, hcs2, hcc1;
    uint32_t caplen;
    uint32_t rts_off, db_off;
    uint32_t i;
    uint32_t tmo;

    cap = xhci_r32_cap(hcd, XHCI_CAPLENGTH);
    caplen = cap & 0xFFU;
    hcd->opregs = hcd->capbase + caplen;
    hcs1 = xhci_r32_cap(hcd, XHCI_HCSPARAMS1);
    hcs2 = xhci_r32_cap(hcd, XHCI_HCSPARAMS2);
    hcc1 = xhci_r32_cap(hcd, XHCI_HCCPARAMS1);
    /* 上下文布局按 CSZ=1(64B) 硬编码(2026-08-12 实测), 偏离即截断报错。 */
    if (XHCI_HCCPARAMS1_CSZ(hcc1) == 0U) {
        USB_LOG_ERR("HCCPARAMS1 CSZ=0 - context layout mismatch\r\n");
        return -USB_ERR_INVAL;
    }

    hcd->max_slots = (uint8_t)XHCI_HCS1_DEVSLOT_MAX(hcs1);
    hcd->num_ports = (uint8_t)XHCI_HCS1_N_PORTS(hcs1);
    db_off = xhci_r32_cap(hcd, XHCI_DBOFF) & 0xFFFFFFF0U;
    rts_off = xhci_r32_cap(hcd, XHCI_RTSOFF) & 0xFFFFFFF0U;
    hcd->db = hcd->capbase + db_off;
    hcd->rts = hcd->capbase + rts_off;

    if (hcd->num_ports > XHCI_MAX_PORTS) {
        hcd->num_ports = XHCI_MAX_PORTS;
    }
    if (hcd->max_slots > XHCI_MAX_SLOTS) {
        hcd->max_slots = XHCI_MAX_SLOTS;
    }

    usbh_console_printf("[USBH] xHCI: %u ports, %u slots, caplen=%u hcs1=0x%08x (capbase=0x%08lx)\r\n",
           hcd->num_ports, hcd->max_slots, caplen, hcs1, hcd->capbase);

    /* 事件环 */
    hcd->event_trbs = usb_sys_malloc_align(64U, XHCI_EVENT_RING_SIZE * TRB_SIZE);
    hcd->erst = usb_sys_malloc_align(64U, sizeof(struct xhci_erst_seg));
    memset(hcd->event_trbs, 0, XHCI_EVENT_RING_SIZE * TRB_SIZE);
    memset(hcd->erst, 0, sizeof(struct xhci_erst_seg));
    hcd->erst->seg_base_lo = (uint32_t)(uintptr_t)hcd->event_trbs;
    hcd->erst->seg_base_hi = 0;
    hcd->erst->seg_size = XHCI_EVENT_RING_SIZE;
    hcd->event_idx = 0;
    hcd->event_cycle = 1;
    hcd->evt_busy = 0;
    hcd->evt_pending = 0;
    xhci_dcache_clean(hcd->event_trbs, XHCI_EVENT_RING_SIZE * TRB_SIZE);
    xhci_dcache_clean(hcd->erst, sizeof(struct xhci_erst_seg));

    /* 命令环 */
    xhci_ring_init(&hcd->cmd_ring, XHCI_CMD_RING_SIZE);

    /* DCBAA */
    hcd->dcbaa = usb_sys_malloc_align(64U, sizeof(uint64_t) * XHCI_MAX_SLOTS);
    memset(hcd->dcbaa, 0, sizeof(uint64_t) * XHCI_MAX_SLOTS);

    /* Scratchpad Buffers: dcbaa[0] = scratchpad buffer array。
     * u-boot xhci_scratchpad_alloc 对齐; 缺省 dcbaa[0]=0 时 rk3568 DWC3
     * 的 Address Device 命令报 Parameter Error(17)。 */
    {
        uint32_t num_sp = XHCI_HCS2_SPB_MAX(hcs2);
        uint64_t *sp_array;
        uint8_t *sp_buf;

        if (num_sp > 0) {
            sp_array = usb_sys_malloc_align(64U, sizeof(uint64_t) * num_sp);
            sp_buf = usb_sys_malloc_align(4096U, 4096U * num_sp);
            memset(sp_array, 0, sizeof(uint64_t) * num_sp);
            memset(sp_buf, 0, 4096U * num_sp);
            for (i = 0; i < num_sp; i++) {
                sp_array[i] = (uint64_t)(uintptr_t)(sp_buf + i * 4096);
            }
            hcd->dcbaa[0] = (uint64_t)(uintptr_t)sp_array;
            /* sp_array/sp_buf 都必须 flush(u-boot xhci_scratchpad_alloc 同款):
             * dcbaa[0] 的 flush 只覆盖指针本身, sp_array[0] 只写在 D-Cache 时
             * 硬件经 DMA 读到 memset 后未回写的全零 -> 等效 "scratchpad 无效",
             * Address Device 报 Parameter Error 17(2026-09-20 板验)。 */
            xhci_dcache_clean(sp_array, sizeof(uint64_t) * num_sp);
            xhci_dcache_clean(sp_buf, 4096U * num_sp);
        }
    }
    xhci_dcache_clean(hcd->dcbaa, sizeof(uint64_t) * XHCI_MAX_SLOTS);

    /* 先 RESET 控制器 */
    xhci_w32(hcd, XHCI_USBCMD, XHCI_CMD_HCRST);
    tmo = 1000;
    while (tmo--) {
        if (xhci_r32(hcd, XHCI_USBSTS) & XHCI_STS_HCH) {
            break;
        }
        usb_osal_msleep(1);
    }
    if (tmo == 0) {
        USB_LOG_ERR("xHCI HCRST timeout\r\n");
        return -USB_ERR_TIMEOUT;
    }
    usb_osal_msleep(2);

    /* DCBAAP */
    xhci_w32(hcd, XHCI_DCBAAP_LO, (uint32_t)(uintptr_t)hcd->dcbaa);
    xhci_w32(hcd, XHCI_DCBAAP_HI, 0);

    /* 命令环 CRCR */
    xhci_w32(hcd, XHCI_CRCR_HI, 0);
    xhci_w32(hcd, XHCI_CRCR_LO, (uint32_t)(uintptr_t)hcd->cmd_ring.trbs | XHCI_CRCR_LO_RCS);

    /* 事件环 ERST + ERDP */
    xhci_w32_rts(hcd, XHCI_ERSTSZ(0), XHCI_ERSTS_SET(1));
    xhci_w32_rts(hcd, XHCI_ERSTBA_LO(0), (uint32_t)(uintptr_t)hcd->erst);
    xhci_w32_rts(hcd, XHCI_ERSTBA_HI(0), 0);
    xhci_w32_rts(hcd, XHCI_ERDP_LO(0), (uint32_t)(uintptr_t)hcd->event_trbs);
    xhci_w32_rts(hcd, XHCI_ERDP_HI(0), 0);

    /* CONFIG: 使能设备槽位 */
    xhci_w32(hcd, XHCI_CONFIG, hcd->max_slots & XHCI_CONFIG_SLOTS_MASK);

    /* DNCTRL: 清零, 防虚假 Device Notification 事件(u-boot xhci_mem_init 同款) */
    xhci_w32(hcd, XHCI_DNCTRL, 0);

    /* 中断使能 */
    xhci_w32_rts(hcd, XHCI_IMOD(0), 0x000001F4U);
    xhci_w32_rts(hcd, XHCI_IMAN(0), XHCI_IMAN_INTR_PEND | XHCI_IMAN_INTR_ENA);

    /* RUN */
    xhci_w32(hcd, XHCI_USBCMD, XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE);
    tmo = 1000;
    while (tmo--) {
        if (!(xhci_r32(hcd, XHCI_USBSTS) & XHCI_STS_HCH)) {
            break;
        }
        usb_osal_msleep(1);
    }
    if (tmo == 0) {
        USB_LOG_ERR("xHCI run timeout\r\n");
        return -USB_ERR_TIMEOUT;
    }

    /* 端口上电(写后回读校验 + 重试): DWC3 USB2 端口(端口2)在 PHY
     * 480MHz 链路就绪前可能吞掉 PP 写入, 对齐 u-boot roothub
     * SET_FEATURE(PORT_POWER) 的生效行为。 */
    for (i = 1; i <= hcd->num_ports; i++) {
        uint32_t ps = xhci_portsc(hcd, i);
        uint32_t try;

        if (ps & XHCI_PS_PP) {
            continue;
        }
        for (try = 0; try < 5; try++) {
            xhci_portsc_w(hcd, i, ps | XHCI_PS_PP);
            usb_osal_msleep(10);
            if (xhci_portsc(hcd, i) & XHCI_PS_PP) {
                break;
            }
        }
        usbh_console_printf("[USBH] Port %u PORTSC=0x%08x\r\n", i, xhci_portsc(hcd, i));
    }

    hcd->running = 1;
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
    if (hcd->capbase == 0) {
        USB_LOG_ERR("Invalid register base for bus %u\r\n", busid);
        return -USB_ERR_INVAL;
    }

    /* 入口期读数(任何平台写之前): 区分"环境读不到 xHCI 孔径"与
     * "我们的序列把它写死" - 2026-09-19 板验二分用。 */
    usbh_console_printf("[USBH] entry: cap[0]=0x%08x GHWPARAMS1=0x%08x\r\n",
                        *(volatile uint32_t *)hcd->capbase,
                        *(volatile uint32_t *)(hcd->capbase + DWC3_GHWPARAMS1));

    /* 平台层: 时钟/PHY/复位/VBUS + 内存池 + 中断 */
    usb_hc_low_level_init(bus);

    ret = dwc3_host_core_check(hcd);
    if (ret != 0) {
        return ret;
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

    xhci_w32(hcd, XHCI_USBCMD, 0);
    hcd->running = 0;
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

static struct xhci_dev *xhci_get_dev(struct xhci_hcd *hcd, struct usbh_hubport *hport)
{
    uint8_t port = hport->port;

    if (port >= 1 && port <= hcd->num_ports && hcd->port_slot[port] != 0) {
        return xhci_find_dev(hcd, hcd->port_slot[port]);
    }
    return NULL;
}

/* 建立槽位 + 初始 Address Device(BSR=1, addr=0, mps=8) */
static int xhci_setup_slot(struct xhci_hcd *hcd, struct usbh_hubport *hport)
{
    struct xhci_dev *dev;
    uint8_t slot_id = 0;
    uint32_t i;
    int ret;

    if (hport->port < 1 || hport->port > hcd->num_ports) {
        return -USB_ERR_INVAL;
    }
    ret = xhci_cmd_enable_slot(hcd, &slot_id);
    if (ret != 0) {
        USB_LOG_ERR("Enable slot failed, ret=%d\r\n", ret);
        return ret;
    }
    dev = &hcd->devs[slot_id];
    memset(dev, 0, sizeof(*dev));
    dev->in_use = 1;
    dev->slot_id = slot_id;
    dev->hport = hport;
    dev->ep0_maxpacket = 0;
    dev->ep_configured = 0;

    /* xHCI: 输入/输出上下文必须 64 字节对齐(规范 §6.2), struct 内嵌数组
     * 只有 4 字节对齐会导致 Address Device 命令 Parameter Error(17)。 */
    dev->input_ctx = usb_sys_malloc_align(64U, sizeof(uint32_t) * XHCI_INPUT_CTX_WORDS);
    dev->dev_ctx = usb_sys_malloc_align(64U, sizeof(uint32_t) * XHCI_DEV_CTX_WORDS);

    /* DCBAA[slot] = 设备上下文(输出) */
    hcd->dcbaa[slot_id] = (uint64_t)(uintptr_t)dev->dev_ctx;
    xhci_dcache_clean(hcd->dcbaa, sizeof(uint64_t) * XHCI_MAX_SLOTS);
    xhci_dcache_clean(dev->dev_ctx, sizeof(uint32_t) * XHCI_DEV_CTX_WORDS);
    usbh_console_printf("[USBH] dcbaa@0x%08lx slot%u dev_ctx@0x%08lx\r\n",
           (unsigned long)hcd->dcbaa, slot_id, (unsigned long)dev->dev_ctx);

    /* ep0 环 */
    dev->ep_rings[0] = usb_sys_malloc_align(64U, sizeof(struct xhci_ring));
    xhci_ring_init(dev->ep_rings[0], XHCI_TRANSFER_RING_SIZE);
    for (i = 1; i < 31; i++) {
        dev->ep_rings[i] = NULL;
    }

    /* Enable Slot 后硬件应写输出上下文(验证 DCBAA 通路) */
    xhci_dcache_invalidate(dev->dev_ctx, sizeof(uint32_t) * XHCI_DEV_CTX_WORDS);
    usbh_console_printf("[USBH] after EnableSlot: out slot0=0x%08x slot3=0x%08x\r\n",
           dev->dev_ctx[0][0], dev->dev_ctx[0][3]);

    /* 初始 Address Device: BSR=1(SetAddress 前寻址)。ep0 MPS=64 是 u-boot
     * 对 HS/FS 设备的板上真值("USB core guesses at a 64-byte max packet");
     * 规范 §4.3 允许 8/16/32/64。 */
    ret = xhci_cmd_address_device(hcd, dev, 1, 0, 64);
    if (ret != 0) {
        USB_LOG_ERR("Address Device (BSR) failed, ret=%d\r\n", ret);
        dev->in_use = 0;
        return ret;
    }

    hcd->port_slot[hport->port] = slot_id;
    return 0;
}

/* ep0 控制传输 TRB 构造; 同步等待完成 */
static int xhci_queue_control(struct xhci_hcd *hcd, struct xhci_dev *dev,
                              struct usbh_urb *urb)
{
    struct usb_setup_packet *setup = urb->setup;
    struct xhci_ring *ring = dev->ep_rings[0];
    struct xhci_trb trb;
    uint8_t *buf = urb->transfer_buffer;
    uint32_t len = urb->transfer_buffer_length;
    uint32_t dir_in = (setup->bmRequestType & 0x80U) != 0;
    int first_idx;
    uint32_t tmo;

    if (xhci_ring_space(ring) < 3) {
        return -USB_ERR_NOMEM;
    }

    /* Setup TRB(立即数据) */
    memset(&trb, 0, sizeof(trb));
    trb.dw0 = (uint32_t)setup->bmRequestType | ((uint32_t)setup->bRequest << 8) |
              ((uint32_t)setup->wValue << 16);
    trb.dw1 = (uint32_t)setup->wIndex | ((uint32_t)setup->wLength << 16);
    trb.dw2 = TRB2_LEN(8) | TRB2_IRQ(0);
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_SETUP) | TRB3_IDT;
    if (len > 0) {
        trb.dw3 |= dir_in ? TRB3_TRT_IN : TRB3_TRT_OUT;
    }
    first_idx = xhci_ring_enqueue(ring, &trb);
    if (first_idx < 0) {
        return -USB_ERR_NOMEM;
    }
    ring->tds[first_idx].urb = urb;
    ring->tds[first_idx].state = 0;
    ring->tds[first_idx].actual = 0;
    ring->tds[first_idx].error = 0;

    /* Data TRB(可选), OUT 方向先 clean 数据缓冲 */
    if (len > 0) {
        uint32_t remain = len;
        uint32_t off = 0;
        uint8_t first_data = 1;

        if (!dir_in) {
            xhci_dcache_clean(buf, len);
        }
        while (remain > 0) {
            uint32_t chunk = (remain > TRB_MAX_BUFF_SIZE) ? TRB_MAX_BUFF_SIZE : remain;
            uintptr_t buf_addr = (uintptr_t)&buf[off];

            if ((buf_addr & (TRB_MAX_BUFF_SIZE - 1)) + chunk > TRB_MAX_BUFF_SIZE) {
                chunk = TRB_MAX_BUFF_SIZE - (buf_addr & (TRB_MAX_BUFF_SIZE - 1));
            }
            memset(&trb, 0, sizeof(trb));
            trb.dw0 = (uint32_t)buf_addr;
            trb.dw1 = (uint32_t)(buf_addr >> 32);
            trb.dw2 = TRB2_LEN(chunk) | TRB2_IRQ(0);
            trb.dw3 = TRB3_TYPE_SET(first_data ? TRB_TYPE_DATA : TRB_TYPE_NORMAL) |
                      TRB3_CHAIN;
            if (dir_in) {
                trb.dw3 |= TRB3_ISP;
            }
            if (xhci_ring_enqueue(ring, &trb) < 0) {
                return -USB_ERR_NOMEM;
            }
            first_data = 0;
            remain -= chunk;
            off += chunk;
        }
    }

    /* Status TRB(方向与 data 相反, 无数据) */
    memset(&trb, 0, sizeof(trb));
    trb.dw0 = 0;
    trb.dw1 = 0;
    trb.dw2 = TRB2_LEN(0) | TRB2_IRQ(0);
    trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_STATUS);
    if (!dir_in) {
        trb.dw3 |= TRB3_ISP;
    }
    if (xhci_ring_enqueue(ring, &trb) < 0) {
        return -USB_ERR_NOMEM;
    }

    /* 最后一个 TRB 加 IOC */
    ring->trbs[(ring->enqueue + ring->num_trbs - 1) % ring->num_trbs].dw3 |= TRB3_IOC;

    xhci_dcache_clean(ring->trbs, ring->num_trbs * TRB_SIZE);
    xhci_ring_db_ep(hcd, dev->slot_id, 0);

    tmo = 0;
    while (tmo < CTRL_TIMEOUT_MS) {
        xhci_event_process(hcd);
        if (ring->tds[first_idx].state != 0) {
            break;
        }
        usb_osal_msleep(1);
        tmo++;
    }

    if (ring->tds[first_idx].state == 1) {
        urb->actual_length = ring->tds[first_idx].actual;
        if (dir_in && urb->actual_length > 0) {
            xhci_dcache_invalidate(buf, urb->actual_length);
        }
        return 0;
    }
    if (ring->tds[first_idx].state == 2) {
        return ring->tds[first_idx].error;
    }
    return -USB_ERR_TIMEOUT;
}

static int xhci_queue_normal(struct xhci_hcd *hcd, struct xhci_dev *dev,
                             uint32_t ep_index, struct usbh_urb *urb)
{
    struct xhci_ring *ring = dev->ep_rings[ep_index];
    struct xhci_trb trb;
    uint8_t *buf = urb->transfer_buffer;
    uint32_t len = urb->transfer_buffer_length;
    uint32_t remain = len;
    uint32_t off = 0;
    uint32_t num_trbs = 0;
    uint32_t first_idx;
    uint32_t dir_in = (urb->ep->bEndpointAddress & USB2_DIR_IN) != 0;
    uint32_t tmo;
    uint32_t timeout_ms = urb->timeout;
    int ret;

    /* 计算 TRB 数(含 64KB 边界裁剪) */
    {
        uintptr_t a = (uintptr_t)buf;
        uint32_t r = len;

        while (r > 0) {
            uint32_t chunk = (r > TRB_MAX_BUFF_SIZE) ? TRB_MAX_BUFF_SIZE : r;

            if ((a & (TRB_MAX_BUFF_SIZE - 1)) + chunk > TRB_MAX_BUFF_SIZE) {
                chunk = TRB_MAX_BUFF_SIZE - (a & (TRB_MAX_BUFF_SIZE - 1));
            }
            num_trbs++;
            a += chunk;
            r -= chunk;
        }
        if (num_trbs == 0) {
            num_trbs = 1; /* ZLP */
        }
    }

    if (xhci_ring_space(ring) < num_trbs + 1) {
        return -USB_ERR_NOMEM;
    }

    if (len == 0) {
        memset(&trb, 0, sizeof(trb));
        trb.dw0 = 0;
        trb.dw1 = 0;
        trb.dw2 = TRB2_LEN(0) | TRB2_IRQ(0);
        trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_NORMAL) | TRB3_IOC;
        if (dir_in) {
            trb.dw3 |= TRB3_ISP;
        }
        first_idx = (uint32_t)xhci_ring_enqueue(ring, &trb);
        if (first_idx == 0xFFFFFFFFU) {
            return -USB_ERR_NOMEM;
        }
    } else {
        if (!dir_in) {
            xhci_dcache_clean(buf, len);
        }
        first_idx = 0xFFFFFFFFU;
        while (remain > 0) {
            uint32_t chunk = (remain > TRB_MAX_BUFF_SIZE) ? TRB_MAX_BUFF_SIZE : remain;
            uintptr_t buf_addr = (uintptr_t)&buf[off];
            uint8_t last = 0;
            int idx;

            if ((buf_addr & (TRB_MAX_BUFF_SIZE - 1)) + chunk > TRB_MAX_BUFF_SIZE) {
                chunk = TRB_MAX_BUFF_SIZE - (buf_addr & (TRB_MAX_BUFF_SIZE - 1));
            }
            remain -= chunk;
            last = (remain == 0);
            memset(&trb, 0, sizeof(trb));
            trb.dw0 = (uint32_t)buf_addr;
            trb.dw1 = (uint32_t)(buf_addr >> 32);
            trb.dw2 = TRB2_LEN(chunk) | TRB2_IRQ(0);
            trb.dw3 = TRB3_TYPE_SET(TRB_TYPE_NORMAL);
            if (!last) {
                trb.dw3 |= TRB3_CHAIN;
            } else {
                trb.dw3 |= TRB3_IOC;
            }
            if (dir_in) {
                trb.dw3 |= TRB3_ISP;
            }
            idx = xhci_ring_enqueue(ring, &trb);
            if (idx < 0) {
                return -USB_ERR_NOMEM;
            }
            if (first_idx == 0xFFFFFFFFU) {
                first_idx = (uint32_t)idx;
            }
            off += chunk;
        }
    }

    ring->tds[first_idx].urb = urb;
    ring->tds[first_idx].state = 0;
    ring->tds[first_idx].actual = 0;
    ring->tds[first_idx].error = 0;

    xhci_dcache_clean(ring->trbs, ring->num_trbs * TRB_SIZE);
    xhci_ring_db_ep(hcd, dev->slot_id, ep_index);

    if (timeout_ms == 0) {
        return 0; /* 异步: 完成由中断回调 */
    }

    tmo = 0;
    while (tmo < timeout_ms) {
        xhci_event_process(hcd);
        if (ring->tds[first_idx].state != 0) {
            break;
        }
        usb_osal_msleep(1);
        tmo++;
    }

    if (ring->tds[first_idx].state == 1) {
        urb->actual_length = ring->tds[first_idx].actual;
        if (dir_in && urb->actual_length > 0) {
            xhci_dcache_invalidate(buf, urb->actual_length);
        }
        return 0;
    }
    if (ring->tds[first_idx].state == 2) {
        ret = ring->tds[first_idx].error;
        return ret;
    }
    return -USB_ERR_TIMEOUT;
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    struct usbh_hubport *hport;
    struct xhci_hcd *hcd;
    struct xhci_dev *dev;
    struct usb_setup_packet *setup;
    uint32_t ep_index;
    uint8_t mps;
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
        setup = urb->setup;
        if (setup == NULL) {
            return -USB_ERR_INVAL;
        }

        /* SET_ADDRESS: xHCI 由 Address Device 命令完成, 不走 TRB */
        if (setup->bRequest == USB_REQUEST_SET_ADDRESS) {
            uint8_t addr = (uint8_t)(setup->wValue & 0xFFU);

            mps = (uint8_t)(hport->ep0.wMaxPacketSize & 0xFFU);
            if (mps != dev->ep0_maxpacket) {
                ret = xhci_cmd_evaluate_context(hcd, dev, mps);
                if (ret != 0) {
                    return ret;
                }
            }
            if (hport->dev_addr != addr) {
                ret = xhci_cmd_address_device(hcd, dev, 0, addr, mps);
                if (ret != 0) {
                    return ret;
                }
            }
            hport->dev_addr = addr;
            urb->actual_length = 0;
            return 0;
        }

        /* 未寻址的设备: 先 Address Device (BSR=1, addr=0, ep0 MPS=64) */
        if (hport->dev_addr == 0 && dev->ep0_maxpacket == 0) {
            ret = xhci_cmd_address_device(hcd, dev, 1, 0, 64);
            if (ret != 0) {
                return ret;
            }
        }

        /* ep0 MPS 变化 -> Evaluate Context */
        mps = (uint8_t)(hport->ep0.wMaxPacketSize & 0xFFU);
        if (mps != dev->ep0_maxpacket) {
            ret = xhci_cmd_evaluate_context(hcd, dev, mps);
            if (ret != 0) {
                return ret;
            }
        }

        return xhci_queue_control(hcd, dev, urb);
    }

    /* bulk / interrupt */
    ep_index = xhci_ep_index(urb->ep);
    if (ep_index == 0 || ep_index >= 8) {
        return -USB_ERR_INVAL;
    }
    if (!(dev->ep_configured & (1U << ep_index))) {
        uint16_t ep_mps = urb->ep->wMaxPacketSize & 0x7FFU;
        uint8_t dir_in = (urb->ep->bEndpointAddress & USB2_DIR_IN) != 0;
        uint8_t ep_type;
        uint8_t interval = 0;
        uint8_t burst = (uint8_t)((urb->ep->wMaxPacketSize >> 11) & 0x3U);

        if (urb->ep->bmAttributes & 0x3U) {
            return -USB_ERR_INVAL; /* 暂不支持 ISO */
        }
        if ((urb->ep->bmAttributes >> 1) & 0x1U) {
            ep_type = (dir_in ? EP_TYPE_INTR_IN : EP_TYPE_INTR_OUT);
            interval = (uint8_t)((urb->ep->bInterval ? (urb->ep->bInterval - 1) : 0) & 0x1FU);
        } else {
            ep_type = (dir_in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT);
        }

        dev->ep_rings[ep_index] = usb_sys_malloc_align(64U, sizeof(struct xhci_ring));
        xhci_ring_init(dev->ep_rings[ep_index], XHCI_TRANSFER_RING_SIZE);

        ret = xhci_cmd_configure_ep(hcd, dev, ep_index, ep_type, ep_mps, interval,
                                    burst, dev->ep_rings[ep_index]);
        if (ret != 0) {
            USB_LOG_ERR("Configure EP%d failed, ret=%d\r\n", ep_index, ret);
            return ret;
        }
    }

    return xhci_queue_normal(hcd, dev, ep_index, urb);
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    /* 同步模型下无在途中断传输; 异步传输由事件环完成/超时收尾。 */
    return 0;
}

/* ===================== Root Hub ===================== */

static void xhci_port_status(struct xhci_hcd *hcd, uint32_t port,
                             uint16_t *status, uint16_t *change)
{
    uint32_t ps = xhci_portsc(hcd, port);
    uint16_t st = 0;
    uint16_t ch = 0;
    uint32_t speed;

    if (ps & XHCI_PS_CCS) {
        st |= 0x0001U; /* CONNECTION */
    }
    if (ps & XHCI_PS_PED) {
        st |= 0x0002U; /* ENABLE */
    }
    if (XHCI_PS_PLS_GET(ps) == 0x3U) {
        st |= 0x0004U; /* SUSPEND */
    }
    if (ps & XHCI_PS_OCA) {
        st |= 0x0008U; /* OVERCURRENT */
    }
    if (ps & XHCI_PS_PR) {
        st |= 0x0010U; /* RESET */
    }
    if (ps & XHCI_PS_PP) {
        st |= 0x0100U; /* POWER */
    }
    speed = XHCI_PS_SPEED_GET(ps);
    if (speed == XHCI_PS_SPEED_HIGH) {
        st |= 0x0400U; /* HIGH_SPEED */
    } else if (speed == XHCI_PS_SPEED_LOW) {
        st |= 0x0200U; /* LOW_SPEED */
    }

    if (ps & XHCI_PS_CSC) {
        ch |= 0x0001U;
    }
    if (ps & XHCI_PS_PEC) {
        ch |= 0x0002U;
    }
    if (ps & XHCI_PS_PLC) {
        ch |= 0x0004U;
    }
    if (ps & XHCI_PS_OCC) {
        ch |= 0x0008U;
    }
    if (ps & XHCI_PS_PRC) {
        ch |= 0x0010U;
    }
    if (ps & XHCI_PS_WRC) {
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
    if (port < 1 || port > hcd->num_ports) {
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
            /* 端口复位: 写 PR 后轮询 PRC(复位完成变化位)。DWC3 USB2 端口
             * 在 PHY 链路未就绪时可能延迟完成, 超时 1s 报错不挂死。 */
            ps = xhci_portsc(hcd, port);
            xhci_portsc_w(hcd, port, ps | XHCI_PS_PR);
            {
                uint32_t tmo;

                for (tmo = 0U; tmo < 1000U; tmo++) {
                    if (xhci_portsc(hcd, port) & XHCI_PS_PRC) {
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
        /* 注意: PORTSC 是 W1C + RW 混合寄存器, 读-改-写会闯祸:
         *  - PED(bit1) 是 RW1CS —— 把快照里的 PED=1 写回会直接禁用端口
         *    (2026-09-20 板验: 端口掉到 PED=0/PLS=7, 之后 Address Device
         *    一律 Parameter Error 17);
         *  - PP(bit9) 是 RW, 写 0 会关端口电源;
         *  - PLS 只在 LWS=1 时可写。
         * 因此只写目标 W1C 位 + 保留 PP; PLS 修改另带 LWS。 */
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

    if (busid >= CONFIG_USBHOST_MAX_BUS) {
        return;
    }
    hcd = &g_xhci[busid];
    if (!hcd->running) {
        return;
    }
    /* 清中断 pending(level 模式, 事件处理完 ERDP 更新后自动解除) */
    xhci_w32_rts(hcd, XHCI_IMAN(0), XHCI_IMAN_INTR_PEND | XHCI_IMAN_INTR_ENA);
    xhci_event_process(hcd);
}
