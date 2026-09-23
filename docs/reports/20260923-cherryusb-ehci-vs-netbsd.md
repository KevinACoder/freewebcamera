# CherryUSB EHCI 本地修改报告 —— 相对 upstream 的完整 patch 与 NetBSD EHCI 对照理由

日期：2026-09-23
基线：CherryUSB upstream master `0e40349b4b5615f019f9d34c1ac92a7b23433a94`
（imports.md 登记基线；本地参考克隆 `cherryusb-upstream` 已切到该 commit）
本地树：`freewebcamera/third-party/cherryusb/`
对照物：NetBSD-11 `sys/dev/usb/ehci.c` / `usbdi.c` / `usbdivar.h`
（rk3568_lab/os/netbsd/src @ `fd359ec1052f`，行号引用即此树）
patch 全文见文末（735 行，6 文件）；独立 patch 文件按需在仓库外分发（不随仓库携带，避免与在跑的调试线互相干扰）。
适用分支：`wip/urtwn-usb` / `wip/rtw8189f-sdio`（当前同点 `6e50c06`）。

## 一、修改总表

| # | 文件 | 修改 | 动机（upstream 缺陷/局限） | NetBSD EHCI/usbdi 对应物 | 状态 |
|---|---|---|---|---|---|
| A | `common/usb_hc.h` `core/usbh_core.{c,h}` | 通用 HCD ops 分发（`usbh_hcd_ops` + per-bus 路由，`CONFIG_USBHOST_MULTI_HCD` 门控） | upstream 一份镜像只允许一个静态 HCD 符号集；本板 EHCI(bus0/1) + xHCI(bus2/3) 并存 | autoconf 多实例：每控制器一份 `ehci_softc`/`xhci_softc`，`cfattach` 天然多挂 | 可上游化（maintainer 将合入） |
| B | `port/ehci/usb_hc_ehci.{c,h}` | `usbh_kill_urb` 不再线程上下文 W1C 清 `USBSTS.IAA`；`qh->killed` 软件旗标与 IAA 池扫描握手，killer 负责 free+complete 恰一次，IAA 丢失时限内联回退 | 线程清 IAA 会 (a) 制造 spurious 中断源；(b) 与 handler 竞争时双完成/漏 QH | `ehci_abort_xfer`：abort 状态机 + `usb_schedsoftintr`，完成回调（`ehci_idone` 路径）恰一次由 softint 侧收尾，killer 不直接碰控制器 Doorbell 语义 | 已随 09-11 线下反馈进入 upstream 讨论；可上游化 |
| C | `port/ehci/usb_hc_ehci.c` | **bulk OUT toggle 全软件化**：QH.DTC=1，提交时把起始 toggle 写入每枚 qTD token 的 DT 位并按 ceil(len/mps) 逐枚推进；`ehci_check_qh` 对 bulk OUT 不再从 retired qTD token 读回 | EHCI spec **不要求**控制器把最终 toggle 写回 retired qTD 的 DT 位；upstream 读回恒得写入初值 0 → 经宿主把 pipe toggle 每笔重置 DATA0 → 第二笔起设备"ACK 但丢弃"（RTL8188EUS iperf3 ~1.2MB 停摆的机制，eb2f723 板测定案） | **持久 QH + overlay**：`ehci_open`（ehci.c:2005）为每 pipe 分配常驻 QH；完成路径 `ehci_set_qh_toggle`（ehci.c:2274-2286）从 **QH overlay 的 qtd_status** 读回硬件维护的 toggle 存入 `nexttoggle`，下一链续用——upstream 的"读 retired qTD"位置本身就没选对 | 可上游化 |
| D | `port/ehci/usb_hc_ehci.c` | **per-endpoint bulk FIFO**（C1，`ehci_bulk_urb_gate`/`ehci_bulk_ep_release`/`ehci_bulk_ep_unpark`）：同端点同时至多一个 QH 在异步环；后续 URB 排队，QH 退役（waitup+kill 两路）弹队首补位，toggle 由退役 URB 携带 | upstream 每 URB 新建 QH 且 `ehci_qh_add_head` 插异步环**头**：多笔在途时控制器 newest-first/交错服务，同端点 bulk 序被破坏（88E 固件按到达序出帧 → 空口帧序混乱 = 持续 TX 坍缩根因，b3dcd08 板测定案） | **一 pipe 一 QH + usbdi 管道 FIFO**：`pipe->up_queue`（usbdivar.h:271）`SIMPLEQ_INSERT_TAIL` 入队（usbdi.c:435）、完成 `SIMPLEQ_REMOVE_HEAD` + `usbd_start_next`（:465-467）——同端点顺序由"每端点同时只有一个 xfer 在硬件上"免费保证；跨端点顺序无关（设备侧各端点独立 FIFO） | 可上游化 |
| E | `common/usb_hc.h` + `port/ehci/usb_hc_ehci.c` | **`USBH_URB_ZERO_PACKET`**（transfer_flags 位）：bulk OUT 长度为 mps 整倍数时在 qTD 链尾追加零长 qTD（IOC 随迁到 ZLP） | upstream 无任何 short-packet/ZLP 语义；宿主 urtwn 的 92E TX 路径 pad=0 且按 `USBD_FORCE_SHORT_XFER` 传参，该语义被静默丢弃 | usbdi flags 原生传递到 HCD：`ehci_alloc_sqtd_chain`（ehci.c:3095、3253-3257）`if (!isread && (flags & USBD_FORCE_SHORT_XFER) && length % mps == 0) { /* Force a 0 length transfer at the end. */` ——我们的实现与它逐字等价 | 可上游化 |
| F | `osal/usb_osal_freertos.c` | `usb_osal_sem_create` max 1→0xFFFF | 计数信号量被当二值用，批量 give 丢失（IAA/完成事件并发路径） | N/A（OS 抽象层；NetBSD 用 cv/锁不用计数语义） | 纯本地 |

不在本 patch 范围（但与本报告相关的配套）：`usb_config.h`（QH 池 16→32、QTD 池同步）在 net80211 镜像仓的 `port/bus/usb/cherryusb/`（项目自有配置文件，非 vendored 改动）；shim 侧 `USBD_FORCE_SHORT_XFER` 置位与 CLEAR_FEATURE(ENDPOINT_HALT) 恢复在 `net_80211` 仓（自有代码，非 vendor）。

## 二、核心对照：为什么 upstream 的形态出问题、NetBSD 的形态不出问题

upstream 的 bulk 传输模型是"**一 URB 一 QH、生命周期随 URB**"：

```
submit: 新 QH ← 填 epchar/toggle ← qTD 链 ← 插异步环头 ← ASEN
complete: 扫描 qTD → IAA 摘挂 → free QH → 回调
```

NetBSD 的模型是"**一 pipe 一 QH、QH 常驻、一 pipe 同时至多一个 xfer 在硬件**"：

```
open: ehci_open 分配 sqh，常驻 async ring
transfer: usbdi pipe->up_queue FIFO 出队一个 xfer → 往常驻 QH 挂 qTD 链
complete: qTD 退休 → 从 QH overlay 取 toggle/ping → free qTD → usbdi 出队下一个
```

三个由此派生的行为差，对应本 patch 的 C/D/E：

1. **同端点顺序**（D）。NetBSD 一 pipe 同时只有一个 xfer 进硬件，顺序天然成立；
   端点级 FIFO 需要软件排队。upstream 把 FIFO 语义丢给了"调用者别并发提交"，
   而异步 urb API 的正确姿势恰恰是允许并发。88E 固件按 bulk OUT 到达序出帧，
   乱序直接上空口（AP 侧重放窗丢帧）——持续 TX 坍缩的根因。
2. **toggle 归属**（C）。持久 QH 下 toggle 活在 QH overlay 里由硬件维护，
   NetBSD 完成时从 overlay 读回存 `nexttoggle` 即可，qTD 的 DT 位可以全零
   （DTC=0）。upstream 每 URB 重建 QH，overlay 不跨 URB 存续，它却仍从 retired
   qTD token 读 toggle——那个位置 spec 根本不保证回写。既然我们的 QH 不持久，
   就把 toggle 彻底搬进软件（DTC=1 + qTD 逐枚盖章 + 由 FIFO 补位路径携带），
   与 NetBSD overlay 读回在语义上等价：**下一个传输的起始 toggle = 上一个的终值**。
3. **短包语义**（E）。NetBSD usbdi 把 `USBD_FORCE_SHORT_XFER` 带进 HCD 并在
   qTD 分配时多算一枚零长 qTD；upstream 连承载字段都没有。加 `USBH_URB_ZERO_PACKET`
   是 Linux `URB_ZERO_PACKET` 同形的最小接口。

**kill 路径（B）的对照**：NetBSD abort 走 `ehci_abort_xfer` 软件状态机，
完成回调恰一次在 softint 侧收尾；upstream 的 kill 在线程上下文直接写
`USBSTS.IAA`——写 1 清 0 的 Doorbell 位被线程 W1C 会吞掉 handler 正在等的
电平（spurious 源 + 抢先消费时"跳回调/漏 QH"）。`qh->killed` 握手把"谁负责
free+complete"收敛成恰好一方，IAA 丢失时限内联回退兜底。

**多控制器（A）的对照**：NetBSD 靠 autoconf 每控制器一实例；upstream 的
全局符号单 HCD 假设本板四条 USB 总线（EHCI×2 + xHCI×2）无法容纳。ops 表 +
per-bus 路由是 upstream 语境下的最小对应物。

## 三、与"移植 NetBSD EHCI 驱动"路线的关系

本 patch 是**演化路线**：保留 upstream 的每 URB QH 模型，把 NetBSD 的三条
语义（端点 FIFO、toggle 归属、短包）补回来。若后续速率归因证明每帧
arm/IAA 往返仍是瓶颈，下一步是把 D 从"软件排队"推进为 NetBSD 形态的
"**持久 QH 常驻 + qTD 链续挂**"（省掉每帧摘挂），这一步仍在本 port 内；
整建制移植 NetBSD ehci.c 作为对照验证线（xHCI 先例 D38）仅在演化路线
证伪时启动，届时本 patch 的 C/D/E 段落即为其需求清单。

## 四、证据链

- toggle 停摆：`docs/evidence/20260922-bulkout-toggle-iperf-fix.md`
- RA 钩子归因与固件版本排除：`docs/evidence/20260923-iperf3-sustained-tx-wedge.md`
- FIFO 消除持续坍缩：`docs/evidence/20260923-layer2b-bulkfifo-collapse-fixed.md`
- imports.md 登记：CherryUSB 行【2026-09-22 追加/追加 2】【2026-09-23 追加 3】

---
以下为本 patch 全文（`a/` = upstream `0e40349b`，`b/` = 本仓 `third-party/cherryusb`）：

```diff
diff -ruN /tmp/tmp.vKxKVTqxpz/a/common/usb_hc.h /tmp/tmp.vKxKVTqxpz/b/common/usb_hc.h
--- a/common/usb_hc.h
+++ b/common/usb_hc.h
@@ -58,6 +58,43 @@
 #endif
 };
 
+/* transfer_flags: bulk OUT ends with an explicit zero-length packet when
+ * the transfer length is an exact multiple of the endpoint max packet
+ * size (USBD_FORCE_SHORT_XFER / URB_ZERO_PACKET semantics) */
+#define USBH_URB_ZERO_PACKET (1u << 0)
+
+/**
+ * @brief USB host controller operations.
+ *
+ * With CONFIG_USBHOST_MULTI_HCD a controller port exposes one of these
+ * tables and binds it per bus through usbh_hcd_register(); core then routes
+ * the plain usb_hc_init / usbh_submit_urb / ... symbols per bus, so several
+ * different HCD ports can link into one image. Without the macro the table
+ * is unused and a port keeps defining those plain symbols directly (the
+ * legacy single-HCD contract, unchanged).
+ */
+struct usbh_hcd_ops {
+    const char *driver_name;
+    int (*hc_init)(struct usbh_bus *bus);
+    int (*hc_deinit)(struct usbh_bus *bus);
+    uint16_t (*get_frame_number)(struct usbh_bus *bus);
+    int (*roothub_control)(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf);
+    int (*submit_urb)(struct usbh_urb *urb);
+    int (*kill_urb)(struct usbh_urb *urb);
+    void (*irq)(uint8_t busid);
+};
+
+/**
+ * @brief Bind an HCD operations table to a bus (multi-HCD builds only).
+ *
+ * Must be called after usbh_initialize() and before the hub thread runs
+ * usb_hc_init() on that bus.
+ *
+ * @param busid The bus to bind to.
+ * @param ops The controller's operations table.
+ */
+void usbh_hcd_register(uint8_t busid, const struct usbh_hcd_ops *ops);
+
 /**
  * @brief usb host controller hardware init.
  *
diff -ruN /tmp/tmp.vKxKVTqxpz/a/core/usbh_core.c /tmp/tmp.vKxKVTqxpz/b/core/usbh_core.c
--- a/core/usbh_core.c
+++ b/core/usbh_core.c
@@ -634,7 +634,14 @@
 
 static void usbh_bus_init(struct usbh_bus *bus, uint8_t busid, uintptr_t reg_base)
 {
+    /* A multi-HCD port registers its ops BEFORE usbh_initialize: on SMP the
+     * hub thread starts running usb_hc_init (through the dispatcher) the
+     * moment usbh_hub_initialize creates it, so registering later is a
+     * race. Keep the pre-registration across the wipe below. */
+    const struct usbh_hcd_ops *ops = bus->hcd_ops;
+
     memset(bus, 0, sizeof(struct usbh_bus));
+    bus->hcd_ops = ops;
     bus->busid = busid;
     bus->hcd.hcd_id = busid;
     bus->hcd.reg_base = reg_base;
@@ -694,6 +701,52 @@
     return 0;
 }
 
+#ifdef CONFIG_USBHOST_MULTI_HCD
+/* Multi-HCD routing: several controller ports (e.g. EHCI + xHCI) provide
+ * their entry points under prefixed names plus a struct usbh_hcd_ops table,
+ * and the plain symbols below dispatch per bus. Without the macro these
+ * dispatchers do not exist and a port's own plain symbols are called
+ * directly, exactly as before. */
+
+void usbh_hcd_register(uint8_t busid, const struct usbh_hcd_ops *ops)
+{
+    USB_ASSERT_MSG(busid < CONFIG_USBHOST_MAX_BUS, "bus overflow\r\n");
+    USB_ASSERT_MSG(ops != NULL, "hcd ops is NULL\r\n");
+
+    g_usbhost_bus[busid].hcd_ops = ops;
+}
+
+int usb_hc_init(struct usbh_bus *bus)
+{
+    return bus->hcd_ops->hc_init(bus);
+}
+
+int usb_hc_deinit(struct usbh_bus *bus)
+{
+    return bus->hcd_ops->hc_deinit(bus);
+}
+
+uint16_t usbh_get_frame_number(struct usbh_bus *bus)
+{
+    return bus->hcd_ops->get_frame_number(bus);
+}
+
+int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf)
+{
+    return bus->hcd_ops->roothub_control(bus, setup, buf);
+}
+
+int usbh_submit_urb(struct usbh_urb *urb)
+{
+    return urb->hport->bus->hcd_ops->submit_urb(urb);
+}
+
+int usbh_kill_urb(struct usbh_urb *urb)
+{
+    return urb->hport->bus->hcd_ops->kill_urb(urb);
+}
+#endif
+
 int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup, uint8_t *buffer)
 {
     struct usbh_urb *urb;
diff -ruN /tmp/tmp.vKxKVTqxpz/a/core/usbh_core.h /tmp/tmp.vKxKVTqxpz/b/core/usbh_core.h
--- a/core/usbh_core.h
+++ b/core/usbh_core.h
@@ -200,6 +200,7 @@
     usb_slist_t list;
     uint8_t busid;
     struct usbh_hcd hcd;
+    const struct usbh_hcd_ops *hcd_ops; /* multi-HCD: controller routing, set by usbh_hcd_register() */
     struct usbh_devaddr_map devgen;
     usb_osal_thread_t hub_thread;
     usb_osal_mq_t hub_mq;
diff -ruN /tmp/tmp.vKxKVTqxpz/a/osal/usb_osal_freertos.c /tmp/tmp.vKxKVTqxpz/b/osal/usb_osal_freertos.c
--- a/osal/usb_osal_freertos.c
+++ b/osal/usb_osal_freertos.c
@@ -42,7 +42,11 @@
 
 usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
 {
-    return (usb_osal_sem_t)xSemaphoreCreateCounting(1, initial_count);
+    /* max 1 would drop every give past the first: callers such as the
+     * usbdi completion ring give once per completed transfer, and a
+     * batch of completions must each wake the worker.  Match the
+     * unbounded counting semantics of the other OS ports. */
+    return (usb_osal_sem_t)xSemaphoreCreateCounting(0xFFFFU, initial_count);
 }
 
 usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count)
diff -ruN /tmp/tmp.vKxKVTqxpz/a/port/ehci/usb_hc_ehci.c /tmp/tmp.vKxKVTqxpz/b/port/ehci/usb_hc_ehci.c
--- a/port/ehci/usb_hc_ehci.c
+++ b/port/ehci/usb_hc_ehci.c
@@ -8,6 +8,34 @@
 #include "usb_hc_ohci.h"
 #endif
 
+#ifdef CONFIG_USBHOST_MULTI_HCD
+/* Multi-HCD build: rename this port's global entry points so other HCD
+ * ports can link alongside it; usbh_core.c dispatches per bus through
+ * usbh_ehci_ops. The renames sit after the includes on purpose - the
+ * declarations above then double as prototypes for the new names, and
+ * usbh_core.h's "#ifdef USBH_IRQHandler #error" guard has already been
+ * evaluated. Function bodies below are untouched. */
+#define usb_hc_init            usbh_ehci_hc_init
+#define usb_hc_deinit          usbh_ehci_hc_deinit
+#define usbh_get_frame_number  usbh_ehci_get_frame_number
+#define usbh_roothub_control   usbh_ehci_roothub_control
+#define usbh_submit_urb        usbh_ehci_submit_urb
+#define usbh_kill_urb          usbh_ehci_kill_urb
+#define USBH_IRQHandler        usbh_ehci_irq
+
+/* Prototypes for the renamed names: the declarations in usb_hc.h were
+ * parsed under the old names before these macros existed, and the
+ * self-call in usbh_ehci_submit_urb plus the ops table below need the
+ * real types in scope. */
+int usbh_ehci_hc_init(struct usbh_bus *bus);
+int usbh_ehci_hc_deinit(struct usbh_bus *bus);
+uint16_t usbh_ehci_get_frame_number(struct usbh_bus *bus);
+int usbh_ehci_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf);
+int usbh_ehci_submit_urb(struct usbh_urb *urb);
+int usbh_ehci_kill_urb(struct usbh_urb *urb);
+void usbh_ehci_irq(uint8_t busid);
+#endif
+
 #define EHCI_TUNE_CERR    3 /* 0-3 qtd retries; 0 == don't stop */
 #define EHCI_TUNE_RL_HS   4 /* nak throttle; see 4.9 */
 #define EHCI_TUNE_RL_TT   0
@@ -27,6 +55,108 @@
 /* The frame list */
 USB_NOCACHE_RAM_SECTION uint32_t g_framelist[CONFIG_USBHOST_MAX_BUS][USB_ALIGN_UP(CONFIG_USB_EHCI_FRAME_LIST_SIZE, 1024)] __attribute__((aligned(4096)));
 
+/* --- per-endpoint bulk FIFO ---------------------------------------------- *
+ *
+ * Bulk URBs on one endpoint must reach the device in submission order:
+ * NetBSD usbdi serves every pipe strictly FIFO, while this port builds a
+ * fresh QH per URB and inserts it at the async ring head, so with several
+ * URBs in flight the controller serves them newest-first - and the
+ * RTL8188E firmware emits frames in bulk-out arrival order, so a reordered
+ * ring shows up on the air as reordered frames.  Only one QH per endpoint
+ * therefore lives in the ring at a time; later URBs park here in
+ * submission order and are armed as each predecessor retires, with the
+ * data toggle carried over from the retiring URB (the QH-overlay
+ * semantics a persistent per-pipe QH gives NetBSD). */
+
+#define EHCI_BULK_FIFO_DEPTH 8
+#define EHCI_BULK_EP_SLOTS   12
+
+struct ehci_bulk_ep_fifo {
+    const struct usb_endpoint_descriptor *ep; /* park key: urb->ep */
+    struct usbh_urb *parked[EHCI_BULK_FIFO_DEPTH];
+    uint8_t head;
+    uint8_t count;
+    bool in_ring; /* an armed QH (or one being armed) holds the gate */
+};
+
+static struct ehci_bulk_ep_fifo ehci_bulk_fifos[EHCI_BULK_EP_SLOTS];
+
+/* gate result for the submit path */
+#define EHCI_BULK_GATE_ARM  0  /* arm this urb now */
+#define EHCI_BULK_GATE_PARK 1  /* parked behind the endpoint's active QH */
+#define EHCI_BULK_GATE_FULL (-1) /* park list exhausted: report NOMEM */
+
+/* caller holds a critical section */
+static struct ehci_bulk_ep_fifo *ehci_bulk_ep_find(const struct usb_endpoint_descriptor *ep)
+{
+    for (uint32_t i = 0; i < EHCI_BULK_EP_SLOTS; i++) {
+        if (ehci_bulk_fifos[i].ep == ep) {
+            return &ehci_bulk_fifos[i];
+        }
+    }
+    return NULL;
+}
+
+static int ehci_bulk_urb_gate(struct usbh_urb *urb)
+{
+    const struct usb_endpoint_descriptor *ep = urb->ep;
+    struct ehci_bulk_ep_fifo *f;
+    size_t flags;
+
+    flags = usb_osal_enter_critical_section();
+    f = ehci_bulk_ep_find(ep);
+    if (f == NULL || (!f->in_ring && f->count == 0U)) {
+        if (f == NULL) {
+            for (uint32_t i = 0; i < EHCI_BULK_EP_SLOTS; i++) {
+                if (ehci_bulk_fifos[i].ep == NULL) {
+                    f = &ehci_bulk_fifos[i];
+                    f->ep = ep;
+                    f->head = 0U;
+                    f->count = 0U;
+                    f->in_ring = false;
+                    break;
+                }
+            }
+        }
+        if (f != NULL) {
+            f->in_ring = true; /* the gate belongs to this urb until release */
+        }
+        usb_osal_leave_critical_section(flags);
+        /* no free slot: degrade to un-gated rather than fail the urb */
+        return EHCI_BULK_GATE_ARM;
+    }
+    if (f->count < EHCI_BULK_FIFO_DEPTH) {
+        f->parked[(f->head + f->count) % EHCI_BULK_FIFO_DEPTH] = urb;
+        f->count++;
+        usb_osal_leave_critical_section(flags);
+        return EHCI_BULK_GATE_PARK;
+    }
+    usb_osal_leave_critical_section(flags);
+    return EHCI_BULK_GATE_FULL;
+}
+
+/* remove a still-parked urb from the fifo; caller holds a critical section */
+static bool ehci_bulk_ep_unpark(struct usbh_urb *urb)
+{
+    struct ehci_bulk_ep_fifo *f = ehci_bulk_ep_find(urb->ep);
+
+    if (f == NULL) {
+        return false;
+    }
+    for (uint8_t i = 0; i < f->count; i++) {
+        if (f->parked[(f->head + i) % EHCI_BULK_FIFO_DEPTH] == urb) {
+            for (uint8_t j = i; j < (uint8_t)(f->count - 1U); j++) {
+                f->parked[(f->head + j) % EHCI_BULK_FIFO_DEPTH] =
+                    f->parked[(f->head + j + 1U) % EHCI_BULK_FIFO_DEPTH];
+            }
+            f->parked[(f->head + f->count - 1U) % EHCI_BULK_FIFO_DEPTH] = NULL;
+            f->count--;
+            return true;
+        }
+    }
+    return false;
+}
+
 static struct ehci_qtd_hw *ehci_qtd_alloc(struct usbh_bus *bus)
 {
     struct ehci_qtd_hw *qtd;
@@ -227,7 +357,12 @@
     epchar |= (dev_addr << QH_EPCHAR_DEVADDR_SHIFT);
     epchar |= (ep_mps << QH_EPCHAR_MAXPKT_SHIFT);
 
-    if (ep_type == USB_ENDPOINT_TYPE_CONTROL) {
+    /* DTC=1: take the toggle from the qTD.  Control always; bulk OUT too,
+     * because a fresh QH is built per transfer and the controller does not
+     * write the final toggle back to the retired qTD token, so software
+     * must own the toggle chain */
+    if (ep_type == USB_ENDPOINT_TYPE_CONTROL ||
+        (ep_type == USB_ENDPOINT_TYPE_BULK && (ep_addr & 0x80) == 0)) {
         epchar |= QH_EPCHAR_DTC; /* toggle from qtd */
     }
 
@@ -424,7 +559,7 @@
     return qh;
 }
 
-static struct ehci_qh_hw *ehci_bulk_urb_init(struct usbh_bus *bus, struct usbh_urb *urb, uint8_t *buffer, uint32_t buflen)
+static struct ehci_qh_hw *ehci_bulk_urb_arm(struct usbh_bus *bus, struct usbh_urb *urb, uint8_t *buffer, uint32_t buflen)
 {
     struct ehci_qh_hw *qh = NULL;
     struct ehci_qtd_hw *qtd = NULL;
@@ -433,6 +568,11 @@
     uint32_t xfer_len = 0;
     uint32_t token;
     size_t flags;
+    /* bulk OUT keeps the data toggle in software (QH.DTC=1): urb->data_toggle
+     * is the starting toggle and is advanced here across the queued packets */
+    uint8_t is_out = ((urb->ep->bEndpointAddress & 0x80) == 0);
+    uint8_t toggle = (uint8_t)urb->data_toggle;
+    uint16_t ep_mps = USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize);
 
     qh = ehci_qh_alloc(bus);
     if (qh == NULL) {
@@ -472,6 +612,12 @@
                  ((uint32_t)EHCI_TUNE_CERR << QTD_TOKEN_CERR_SHIFT) |
                  ((uint32_t)xfer_len << QTD_TOKEN_NBYTES_SHIFT);
 
+        /* stamp this qTD with its starting toggle; with QH.DTC=1 the
+         * controller takes the toggle from here, not from the overlay */
+        if (is_out && toggle) {
+            token |= QTD_TOKEN_TOGGLE;
+        }
+
         if (buflen == 0) {
             token |= QTD_TOKEN_IOC;
         }
@@ -481,6 +627,14 @@
         qtd->hw.next_qtd = QTD_LIST_END;
         buffer += xfer_len;
 
+        /* advance across the packets queued in this qTD; a zero-length qTD
+         * still moves one (zero-length) packet */
+        if (is_out) {
+            uint32_t packets = (xfer_len == 0) ? 1 : ((xfer_len + ep_mps - 1) / ep_mps);
+
+            toggle ^= (uint8_t)(packets & 1);
+        }
+
         if (prev_qtd) {
             prev_qtd->hw.next_qtd = EHCI_PTR2ADDR(qtd);
         } else {
@@ -493,6 +647,32 @@
         }
     }
 
+    /* USBD_FORCE_SHORT_XFER semantics (USBH_URB_ZERO_PACKET): a bulk OUT
+     * whose length is an exact multiple of the max packet size ends with
+     * an explicit zero-length packet; the IOC moves onto it */
+    if (is_out && (urb->transfer_flags & USBH_URB_ZERO_PACKET) &&
+        urb->transfer_buffer_length > 0 &&
+        (urb->transfer_buffer_length % ep_mps) == 0) {
+        qtd = ehci_qtd_alloc(bus);
+        USB_ASSERT_MSG(qtd, "bulk zlp qtd alloc failed");
+
+        token = QTD_TOKEN_PID_OUT | QTD_TOKEN_STATUS_ACTIVE |
+                ((uint32_t)EHCI_TUNE_CERR << QTD_TOKEN_CERR_SHIFT) |
+                QTD_TOKEN_IOC;
+        if (toggle) {
+            token |= QTD_TOKEN_TOGGLE;
+        }
+
+        ehci_qtd_fill(qtd, (uintptr_t)buffer, 0, token);
+        qtd->urb = urb;
+        qtd->hw.next_qtd = QTD_LIST_END;
+
+        prev_qtd->hw.token &= ~QTD_TOKEN_IOC;
+        prev_qtd->hw.next_qtd = EHCI_PTR2ADDR(qtd);
+        prev_qtd = qtd;
+        toggle ^= 1U; /* one zero-length packet */
+    }
+
     /* update qh first qtd */
     qh->hw.curr_qtd = EHCI_PTR2ADDR(first_qtd);
     qh->hw.overlay.next_qtd = EHCI_PTR2ADDR(first_qtd);
@@ -504,6 +684,12 @@
         qh->hw.overlay.token = 0;
     }
 
+    if (is_out) {
+        /* final toggle of a fully-completed transfer, ready for the next
+         * submit; ehci_check_qh must not resync it from the qTD token */
+        urb->data_toggle = toggle;
+    }
+
     /* record qh first qtd */
     qh->first_qtd = EHCI_PTR2ADDR(first_qtd);
 
@@ -520,6 +706,51 @@
     return qh;
 }
 
+/* The endpoint's active bulk QH retired (completed, errored or killed):
+ * hand the gate to the next parked urb and arm it.  Runs before the
+ * completion callback so a driver re-submit from the callback parks
+ * behind the armed urb instead of racing it into the ring. */
+static void ehci_bulk_ep_release(struct usbh_bus *bus, struct usbh_urb *urb)
+{
+    struct ehci_bulk_ep_fifo *f;
+    struct usbh_urb *next = NULL;
+    size_t flags;
+
+    flags = usb_osal_enter_critical_section();
+    f = ehci_bulk_ep_find(urb->ep);
+    if (f != NULL) {
+        if (f->count > 0U) {
+            next = f->parked[f->head];
+            f->parked[f->head] = NULL;
+            f->head = (uint8_t)((f->head + 1U) % EHCI_BULK_FIFO_DEPTH);
+            f->count--;
+            f->in_ring = true; /* hold the gate across the re-arm */
+        } else {
+            f->in_ring = false;
+        }
+    }
+    usb_osal_leave_critical_section(flags);
+
+    if (next != NULL) {
+        /* toggle carry-over: for bulk OUT the retiring urb's data_toggle
+         * is its final toggle (pre-advanced at arm time), for bulk IN it
+         * was resynced from the retired qTD, and a halt forced it to 0 -
+         * exactly what the next transfer must start from */
+        next->data_toggle = urb->data_toggle;
+        if (ehci_bulk_urb_arm(bus, next, next->transfer_buffer,
+                              next->transfer_buffer_length) == NULL) {
+            /* pool exhausted: drop the gate; the parked urb's watchdog
+             * kill (hcpriv == NULL path) reaps it */
+            flags = usb_osal_enter_critical_section();
+            f = ehci_bulk_ep_find(urb->ep);
+            if (f != NULL) {
+                f->in_ring = false;
+            }
+            usb_osal_leave_critical_section(flags);
+        }
+    }
+}
+
 static struct ehci_qh_hw *ehci_intr_urb_init(struct usbh_bus *bus, struct usbh_urb *urb, uint8_t *buffer, uint32_t buflen)
 {
     struct ehci_qh_hw *qh = NULL;
@@ -622,6 +853,14 @@
 
     qh = (struct ehci_qh_hw *)urb->hcpriv;
 
+    /* A killed urb's unlink is acknowledged here, but its free and
+     * complete stay with usbh_kill_urb: the kill needs to deliver its
+     * own -USB_ERR_SHUTDOWN exactly once, from its caller's context. */
+    if (qh->killed == 1U) {
+        qh->killed = 2U;
+        return;
+    }
+
     qh->remove_in_iaad = 0;
 
 #ifdef CONFIG_USB_DCACHE_ENABLE
@@ -647,6 +886,13 @@
         ehci_qh_free(bus, qh);
     }
 
+    if (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
+        /* the endpoint's bulk QH retired: arm the next parked urb
+         * before the completion callback runs, so a driver re-submit
+         * from the callback parks behind it instead of racing it */
+        ehci_bulk_ep_release(bus, urb);
+    }
+
     if (urb->complete) {
         if (urb->errorcode < 0) {
             urb->complete(urb->arg, urb->errorcode);
@@ -656,6 +902,47 @@
     }
 }
 
+/* Wait for the async advance of a killed QH to complete.  The IAA
+ * interrupt belongs to the irq handler: usbh_kill_urb must never
+ * write-clear USBSTS.IAA from thread context, because that can erase
+ * a pending level (the spurious-irq signature) and make the handler's
+ * qh pool scan skip this advance entirely, stranding every other QH
+ * that already set remove_in_iaad.  Instead the handler acknowledges
+ * the killed QH through the qh->killed handshake (see
+ * ehci_urb_waitup); only when the interrupt never arrives does the
+ * killer fall back to finishing the unlink itself.  Exactly one of
+ * the two paths frees the qh and runs the callback. */
+static void usbh_kill_urb_wait_advance(struct usbh_bus *bus, struct usbh_urb *urb,
+                                       struct ehci_qh_hw *qh)
+{
+    volatile uint32_t timeout = 0;
+
+    EHCI_HCOR->usbcmd |= EHCI_USBCMD_IAAD;
+
+    while (qh->killed != 2U) {
+        timeout++;
+        if (timeout > 200000U) {
+            /* IAA lost (or the handler raced us): finish the unlink
+             * here so the urb still completes exactly once. */
+            size_t iflags = usb_osal_enter_critical_section();
+
+            if (qh->killed == 2U) {
+                usb_osal_leave_critical_section(iflags);
+                break;
+            }
+            qh->remove_in_iaad = 0;
+            qh->killed = 0;
+            usb_osal_leave_critical_section(iflags);
+
+            USB_LOG_ERR("iaad lost, finishing killed urb inline\r\n");
+            return;
+        }
+    }
+
+    qh->remove_in_iaad = 0;
+    qh->killed = 0;
+}
+
 static void ehci_qh_scan_qtds(struct usbh_bus *bus, struct ehci_qh_hw *qhead, struct ehci_qh_hw *qh)
 {
     struct ehci_qtd_hw *qtd;
@@ -703,10 +990,17 @@
     urb = qh->urb;
 
     if ((token & QTD_TOKEN_STATUS_ERRORS) == 0) {
-        if (token & QTD_TOKEN_TOGGLE) {
-            urb->data_toggle = true;
-        } else {
-            urb->data_toggle = false;
+        /* bulk OUT toggle is software-managed (QH.DTC=1) and was already
+         * advanced to its final value at submit time; the DT bit of a
+         * retired qTD token is not written back by the controller, so it
+         * must never be resynced from here */
+        if (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) != USB_ENDPOINT_TYPE_BULK ||
+            (urb->ep->bEndpointAddress & 0x80) != 0) {
+            if (token & QTD_TOKEN_TOGGLE) {
+                urb->data_toggle = true;
+            } else {
+                urb->data_toggle = false;
+            }
         }
         urb->errorcode = 0;
     } else {
@@ -1215,6 +1509,7 @@
     struct ehci_qh_hw *qh = NULL;
     size_t flags;
     int ret = 0;
+    int gate;
     struct usbh_hub *hub;
     struct usbh_hubport *hport;
     struct usbh_bus *bus;
@@ -1280,8 +1575,23 @@
             }
             break;
         case USB_ENDPOINT_TYPE_BULK:
-            qh = ehci_bulk_urb_init(bus, urb, urb->transfer_buffer, urb->transfer_buffer_length);
+            gate = ehci_bulk_urb_gate(urb);
+            if (gate == EHCI_BULK_GATE_PARK) {
+                break; /* parked; armed when the active QH retires */
+            }
+            if (gate == EHCI_BULK_GATE_FULL) {
+                return -USB_ERR_NOMEM;
+            }
+            qh = ehci_bulk_urb_arm(bus, urb, urb->transfer_buffer, urb->transfer_buffer_length);
             if (qh == NULL) {
+                /* give the gate back so later urbs are not stranded */
+                flags = usb_osal_enter_critical_section();
+                struct ehci_bulk_ep_fifo *uf = ehci_bulk_ep_find(urb->ep);
+
+                if (uf != NULL) {
+                    uf->in_ring = false;
+                }
+                usb_osal_leave_critical_section(flags);
                 return -USB_ERR_NOMEM;
             }
             break;
@@ -1325,7 +1635,29 @@
     size_t flags;
     bool remove_in_iaad = false;
 
-    if (!urb || !urb->hport || !urb->hcpriv || !urb->hport->bus) {
+    if (!urb || !urb->hport || !urb->hport->bus) {
+        return -USB_ERR_INVAL;
+    }
+
+    if (urb->hcpriv == NULL &&
+        USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
+        /* a parked urb owns no QH: take it off the endpoint fifo and
+         * complete it from here */
+        size_t pflags = usb_osal_enter_critical_section();
+        bool was_parked = ehci_bulk_ep_unpark(urb);
+
+        usb_osal_leave_critical_section(pflags);
+        if (was_parked) {
+            urb->errorcode = -USB_ERR_SHUTDOWN;
+            if (urb->complete) {
+                urb->complete(urb->arg, urb->errorcode);
+            }
+            return 0;
+        }
+        return -USB_ERR_INVAL;
+    }
+
+    if (!urb->hcpriv) {
         return -USB_ERR_INVAL;
     }
 
@@ -1376,28 +1708,56 @@
     EHCI_HCOR->usbcmd |= (EHCI_USBCMD_PSEN | EHCI_USBCMD_ASEN);
 
     qh = (struct ehci_qh_hw *)urb->hcpriv;
-    qh->remove_in_iaad = 0;
+    /* keep remove_in_iaad set when the unlink needs the async advance:
+     * the IAA pool scan must see it for the killed handshake to
+     * complete; the wait clears it afterwards */
+    qh->killed = 1U;
     urb->errorcode = -USB_ERR_SHUTDOWN;
 
+    if (remove_in_iaad) {
+        /* release the critical section before waiting: the
+         * acknowledgement comes from the irq handler, whose own
+         * completion paths need this section, so holding it here
+         * would deadlock the very interrupt we wait for */
+        usb_osal_leave_critical_section(flags);
+        /* ring the doorbell and wait for the handler to acknowledge;
+         * never touch USBSTS.IAA here (see
+         * usbh_kill_urb_wait_advance) */
+        usbh_kill_urb_wait_advance(bus, urb, qh);
+
+        if (urb->timeout) {
+            /* the blocked submitter wakes, reads errorcode and frees
+             * the qh itself (usbh_submit_urb timeout path) */
+            usb_osal_sem_give(qh->waitsem);
+        } else {
+            ehci_qh_free(bus, qh);
+            if (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
+                ehci_bulk_ep_release(bus, urb);
+            }
+        }
+
+        if (urb->complete) {
+            urb->complete(urb->arg, urb->errorcode);
+        }
+        return 0;
+    }
+
     if (urb->timeout) {
+        /* the blocked submitter wakes, reads errorcode and frees the
+         * qh itself (usbh_submit_urb timeout path) */
         usb_osal_sem_give(qh->waitsem);
     } else {
+        /* the urb was already unlink'd by the completion scan and its
+         * advance doorbell may still be in flight: neutralize the
+         * pool scan for this qh before releasing it, or the in-flight
+         * IAA would run waitup a second time (double free, double
+         * complete) */
+        qh->remove_in_iaad = 0;
+        qh->killed = 0U;
         ehci_qh_free(bus, qh);
-    }
-
-    if (remove_in_iaad) {
-        volatile uint32_t timeout = 0;
-        EHCI_HCOR->usbsts = EHCI_USBSTS_IAA;
-        EHCI_HCOR->usbcmd |= EHCI_USBCMD_IAAD;
-        while (!(EHCI_HCOR->usbsts & EHCI_USBSTS_IAA)) {
-            timeout++;
-            if (timeout > 200000) {
-                USB_LOG_ERR("iaad timeout\r\n");
-                usb_osal_leave_critical_section(flags);
-                return -USB_ERR_TIMEOUT;
-            }
+        if (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
+            ehci_bulk_ep_release(bus, urb);
         }
-        EHCI_HCOR->usbsts = EHCI_USBSTS_IAA;
     }
 
     if (urb->complete) {
@@ -1497,4 +1857,17 @@
 
     if (usbsts & EHCI_USBSTS_FATAL) {
     }
-}
\ No newline at end of file
+}
+
+#ifdef CONFIG_USBHOST_MULTI_HCD
+const struct usbh_hcd_ops usbh_ehci_ops = {
+    .driver_name = "ehci",
+    .hc_init = usbh_ehci_hc_init,
+    .hc_deinit = usbh_ehci_hc_deinit,
+    .get_frame_number = usbh_ehci_get_frame_number,
+    .roothub_control = usbh_ehci_roothub_control,
+    .submit_urb = usbh_ehci_submit_urb,
+    .kill_urb = usbh_ehci_kill_urb,
+    .irq = usbh_ehci_irq,
+};
+#endif
\ No newline at end of file
diff -ruN /tmp/tmp.vKxKVTqxpz/a/port/ehci/usb_hc_ehci.h /tmp/tmp.vKxKVTqxpz/b/port/ehci/usb_hc_ehci.h
--- a/port/ehci/usb_hc_ehci.h
+++ b/port/ehci/usb_hc_ehci.h
@@ -64,6 +64,10 @@
     struct usbh_urb *urb;
     usb_osal_sem_t waitsem;
     uint8_t remove_in_iaad;
+    /* usbh_kill_urb handshake with the IAA pool scan: 1 = killed and
+     * waiting for the scan to acknowledge the unlink, 2 = acknowledged
+     * (the scan then leaves free + complete to the killer) */
+    volatile uint8_t killed;
 } __attribute__((aligned(CONFIG_USB_EHCI_ALIGN_SIZE)));
 
 struct ehci_itd_hw {
@@ -95,6 +99,11 @@
 extern uint32_t g_framelist[CONFIG_USBHOST_MAX_BUS][USB_ALIGN_UP(CONFIG_USB_EHCI_FRAME_LIST_SIZE, 1024)];
 extern uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port);
 
+#ifdef CONFIG_USBHOST_MULTI_HCD
+/* This port's routing table for usbh_hcd_register() (multi-HCD builds). */
+extern const struct usbh_hcd_ops usbh_ehci_ops;
+#endif
+
 int ehci_iso_urb_init(struct usbh_bus *bus, struct usbh_urb *urb);
 void ehci_kill_iso_urb(struct usbh_bus *bus, struct usbh_urb *urb);
 void ehci_scan_isochronous_list(struct usbh_bus *bus);
```
