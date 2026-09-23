# 满载 ~13M 失稳两形态 + B1/同步基线的正向观测（2026-09-23 板测）

分支 `wip/urtwn-usb` @ 0e61e87（130f7e3 cherryusb dev 5019680e + 0e61e87 B1 优先级），
镜像 `75434fbd…`（1010904 B）。板卡窗口经用户确认后越锁使用（8821CU 会话在计划中未用板，
锁保留未动）；测后已 `power_off` 复核 `power_on=false`。

## 正向观测（全部 PASS）

1. **同步后枚举/attach 零回归**：urtwn1（RTL8188EU，`claimed 0bda:8179`）正常认领初始化，
   `1 rx pipe, 2 tx pipes`，MAC 20:f4:1b:52:8f:01。
2. **关联一次成功**（golden BSSID 14:a3:2f:18:07:2c）——此前固定形态是 3-7 次重试。
3. **RTT 大幅改善**：ping 192.168.0.18 = 6/4/2/2 ms（R-A 基线 ~11ms；SDIO 线 C5 的
   "7ms min 中 5ms 未解释"在本线直接降到 2ms——与 B1 worker 降优先级同向的证据）。
4. **usbstats dispatch 修复生效**（c8c01fb 首次满载验证）：submit/complete 计数正常走，
   `usbtime tx/rx` 有样本；空闲态 tx 三桶 queue=0/0 hcd=3/38帧 wake=2/38帧（每帧
   ~0.13ms，量级健康）。
5. **短时吞吐已达 NetBSD N6 包络**：第二次尝试 1-2s 区间 **13.99 Mbit/s**（第一次尝试
   0-1s 3.27M=慢启动）。codex 短测 12.74M 同级——**硬件与 88E 固件不是天花板**，
   2.3M→12M+ 的路径存在，剩余问题只有满载稳定性。

## 失稳两形态（满载 ~13M 触发，间歇性，2 次尝试 2 中）

- **形态 A（fatal）**：iperf3 TCP forward 启动即
  `fatal[0] core0 kind1 ESR=96000004 FAR=580008c0 ELR=0a00aa64 LR=0a038b8c sp=0a525f10`。
  addr2line：ELR = `_txe_semaphore_put`（txe_semaphore_put.c:79，读
  `semaphore_ptr->tx_semaphore_id`），LR = `usb_osal_sem_give`
  （usb_osal_threadx.c:138）。**信号量指针值本身是垃圾**（0x580008c0，位型似 EHCI
  qTD token：ACTIVE|HALT|CERR）；shim bulk 走 `timeout=0` 异步（usbdi_compat.c:649），
  waitup 的 waitsem give 不在路径上 → 热路径 give 即 `usbd_ring_post` 的
  `dev->ring_sem`（usbdi_compat.c:275/282，IRQ 上下文）→ **shim 设备结构/池内存被
  硬件形状的值踩踏**。崩前 ring_sem 已成功 give 3937 次。
- **形态 B（静默楔死）**：13.99M 跑到第 2 个 interval 后整机无输出（shell 回显消失，
  串口完全静默）——非单 USB 停摆，是全系统失稳。与 4d5907a 短测（当时是未提交
  HCD 实验形态）"12.74M 后不再产生完成区间"同签名。

## 归因指向（待验证）

两种形态都在满载才触发、间歇性 → **竞态踩内存**，首要嫌疑 = per-urb QH
alloc/free/插环/摘环/IAAD 等待的 churn 路径在 IRQ（waitup/qh_free/scan）与
worker（submit/arm）之间的窗口。B1（worker 5→16）改变了完成-补位时序（完成在
IRQ ring 积批、worker 延后消化），**放大了 R-A 时代（worker=5）未触发的窗口**——
注意 c8c01fb/24a44ea 此前也从未跑过干净满载，bisect 尚未做。churn 本身就是
"每帧节奏"优化的主对象：**稳定性修复与提速合流**。候选修法（契约内，调用方排队不变）：
- per-EP QH 复用（arm 时复用上次 QH 不重分配——NetBSD persistent QH 的语义等价物，
  无 HCD 内排队，不违"一 ep 一笔在途"）；
- 或精确修补 completion free vs submit arm 的互斥窗口（先 USB 源码审查定位）。

## 复现/区分实验清单（下轮）

1. 当前基线第 3 次采样（确认复现率）。
2. B1 回退构建（worker=5）满载采样——若稳定回落 2.3M 则确认"时序暴露竞态"，
   之后仍走修竞态路线（B1 本身是 SDIO 板验的正确方向，不回退终局）。
3. cherryusb-only 回退构建——区分 5019680e 同步 vs 既有缺陷。
4. 修复后：30s×3 双向 ≥10M + 600s soak + usbstats 三桶差值归因（快照 1 已存：
   submit=3938 complete=3937 tx=38 rx=3900，usbtime tx n=38 queue=0/0 hcd=3/1
   wake=2/1；rx n=3900 queue=17/1 hcd=198448/270 wake=18/1）。
