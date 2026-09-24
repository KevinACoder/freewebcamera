# 层 2b 方案重定位：排队上移 usbdi shim，回退 EHCI 内 FIFO（R-A 板验 PASS）

日期：2026-09-23 ｜ 分支：freewebcamera `wip/urtwn-usb` @ 2f84abc ｜ net_80211 `feat-rtw8189f-sdio` @ de406f2 ｜ 镜像 sha256 `a4e7ccddca5cef6fbbd635fcfab10f62c959ed3d60b105118e2733f83ee2c28d`（1009944 B）

## 背景与定论

CherryUSB 社区（用户为 maintainer）对层 2b 修复（b3dcd08，HCD 内 per-endpoint bulk FIFO）
的反馈：**一次给一个 endpoint 多笔 urb 是不允许的；多笔应挂在 urb 的 list 上，完成后再
从 list 取下一笔**——排队是调用方的责任，CherryUSB 按 Linux 的设计走。

上游考证（0e40349b 逐点核实）：

- "一 ep 一笔在途"是**纯约定、代码零强制**：唯一防护是 per-urb 实例的
  `-USB_ERR_BUSY`（port/ehci/usb_hc_ehci.c:1251），拦不住同 ep 第二笔 urb。
- 全部 class/hub 驱动都是"每 endpoint 一个内嵌 urb 成员 + 完成后续投"
  （usbh_msc/usbh_hid/usbh_cdc/usbh_hub 等逐一核对）；`usbh_urb.list` 节点字段
  在 host 栈内零使用——正是社区所说"挂在 urb 的 list 上"的预留形状。
- **违约方在我方 usbdi shim**：`usbd_transfer` 每笔直通 `usbh_submit_urb`
  （usbdi_compat.c:630），而 urtwn `urtwn_start()` 连续出队逐帧提交、每 pipe 预分配
  8 个 xfer（if_urtwnvar.h:26 `URTWN_TX_LIST_COUNT=8`）→ 同 ep 最多 8 笔并发进 EHCI，
  每笔新 QH 头插异步环 → 逆序服务。NetBSD 里这 8 笔由 usbdi 的 `up_queue` FIFO 排序。

## 改动

1. **net_80211（de406f2）**：NetBSD `up_queue` 语义移植进 shim——
   - per-pipe `xq` 队列（尾插 + `xq_busy` 占位标志），`usbd_transfer` 在 pipe 忙时入队；
   - `usbd_pipe_kick`（urb worker 内，成功完成后补位）= NetBSD softint 重启管道的形状；
     持 serializer（可重入）与线程侧提交互斥；
   - **toggle 链由构造保证正确**：pipe 播种只发生在上一笔完成回写之后（或 clear-halt
     复位 DATA0 之后），C1 的 urb→urb toggle 搬运随之作废；
   - **STALL 闸门**：kick 在 clear-halt（540-547 同步执行）之后——顺带修掉 C1 的
     潜在缺陷（FIFO 补位不查 halting）；
   - watchdog 扩到队内 xfer（超时合成 USBD_TIMEOUT，无 urb 可 kill）；
     abort/close 冲刷队列（USBD_CANCELLED）；destroy 防御摘链（q_orphan 恒 0）；
   - 统计：`q_kicks/q_peak/q_flush/q_timeout/q_orphan` 进 usbstats dump。
2. **freewebcamera（2f84abc，wip/urtwn-usb）**：手工回退 b3dcd08 的 FIFO 面
   （FIFO 块/release/waitup 钩子/submit gate/kill parked 分支/kill release 调用），
   **保留** C3（USBH_URB_ZERO_PACKET 零长 qTD；块位置移到 ehci_bulk_urb_arm 内
   overlay toggle 写之后、最终软件 toggle 回写之前，语义不变）与 arm 改名；
   eb2f723 toggle 软件化零改动。vendored diff 回到"可上游化"形态。
   注：镜像同步只带本轮两个文件（usbdi_compat.c/usb_config.h），并行线
   （rtw8189f/lwip_netif）的 net_80211 提交**不**拖入本线。

## 板测 R-A（单变量：排队从 HCD 移到 shim，其余全同 R2 轮）

冷启动（KI-001）→ `wpa connect QiQiJia paopaojie520`（第 2 次关联成功，同已知重试形态）
→ DHCP 192.168.0.10 → ping 192.168.0.18 4/4 @ 11ms → `iperf3 192.168.0.18 30` ×3：

| 轮 | 总量 | 总速率 | interval 形态 |
|---|---|---|---|
| 1 | 8859648 B | **2.36 Mbit/s** | 30/30 全打印，1.96–2.85M 平稳 |
| 2 | 8810496 B | **2.34 Mbit/s** | 30/30 全打印，平稳 |
| 3 | 8794112 B | **2.34 Mbit/s** | 全程平稳（1 行 interval 被串口环缓冲吞，总量守恒） |

三轮全部干净收尾，无停顿、无 watchdog、无需强制断开——**与 C1 轮等效且略优**
（C1 轮 2.03/2.25M）。⇒ 排序不变量属于 usbdi 层；EHCI port 回到上游形状后行为保持。

（过程坑：iperf3 服务器用错老 3.1.4 路径被防火墙拒（errno 103）；换
`D:\Software\iperf3.19.1`（历次板测授权过的版本）后恢复。已记入长期记忆：
**D:\Software = Windows 侧工具目录，iperf3 用 D:\Software\iperf3.19.1**。）

## 结论与遗留

- **结论**：层 2b 的修复责任面从 HCD 收回到调用方（shim），与上游契约对齐；
  vendored CherryUSB 差量收敛为 D50 ops + kill 握手 + osal sem + eb2f723 toggle + C3。
- **遗留（下轮）**：
  1. `wlan usbstats` 零读数复现（连新 q_* 行也全 0）——dump 打到非 urtwn 实例，
     双适配器 dispatch 问题；`wlan reg txq` 同样读到 rtw8189f（SDIO）侧寄存器。
     修掉后用 q_kicks/完成节奏判别 2M 限速（每帧 ~6ms）归因。
  2. 持续速率 ~2.3M vs NetBSD 14M 的差距未动（本轮不改节奏，窗口/环境已排除）。
  3. 关联重试（RTOS 3-7 次 vs NetBSD 秒连）未动。
  4. wip/rtw8189f-sdio 的镜像同步**未做**（并行会话在跑，等各自验收后合并时同步；
     shim 改动在 net_80211 单一集成分支，两侧构建时自然取到）。
  5. clean-room 扫描存量命中：master 的 port/adapters/sdmmc 头文件带
     "Copyright (c) 2026 Phytium"（BSD-3 逐字移植保留上游版权）与 `include/fs.h`
     文件名命中 f 正则——M3-B 存量，非本轮引入；需决策：改门禁排除还是清理头文件。
