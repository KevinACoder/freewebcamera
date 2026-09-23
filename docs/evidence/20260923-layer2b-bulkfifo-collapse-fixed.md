# 层 2b 收口：bulk OUT 每端点 FIFO 消除持续 TX 坍缩 —— 停摆定案修复，速率上限转独立课题

日期：2026-09-23
提交：freewebcamera `b3dcd08`（CherryUSB C1+C3-HCD）+ `9a9df36`/`aHEAD`（net80211 镜像 b7830ed+fix、lwIP 窗口）；
net_80211 `b7830ed` + forward-decl fix（usbdi shim C2/C3-shim/C4）
镜像：`1c70e9f9…`（1010312 B，C1+C2+C3+C4+32×MSS 窗口）；R1 对照镜像 `f9428cce…`（仅 C1 生效、C3-HCD 死码）
dongle：RTL8188EUS `20:f4:1b:52:8f:01`（urtwn1）；iperf3 服务端 192.168.0.18（win64 3.19.1）

## TL;DR

1. **持续 TX 坍缩（层 2b，M7+ 唯一阻塞）已消除**：C1 = EHCI **bulk per-endpoint FIFO**（同端点同时至多一个 QH 在异步环，后续 URB 排队、完成中断里补位 + toggle 携带）。修复前后失败形态质变：修复前"满速突发 → 串口静默 90s+ → 楔死需强制断开"；修复后三轮 30s TCP 全部**逐秒 interval 平稳、正常收尾**。
2. **持续上行速率 ~2-2.25 Mbit/s**（NetBSD 同日同时段 14.3M）：TCP 窗口 8→32×MSS 无改善（BDP 假设排除）、NetBSD 下午复跑 14.3M（环境排除）→ 限制收敛到**每帧发送节奏**（~6ms/帧）：C1 单 QH 串行化的 arm 往返、或 lwIP ACK/输出 cadence（两者未被现有观测区分）。**速率上限为下一轮课题，不再是"测试不过"性质的阻塞**。
3. C2（CLEAR_FEATURE(ENDPOINT_HALT) 恢复）/C3（USBD_FORCE_SHORT_XFER→ZLP qTD）/C4（QH 池 32）随镜像落库，属 NetBSD 等价形态的韧性/语义补齐。
4. 新观测问题（下轮先修）：`wlan usbstats` 在双适配器镜像上恒 0（流量真实流动，疑 M11 的 active-adapter dispatch 覆盖了 urtwn 的 dump）；RTOS 侧 wpa 关联需 3-7 次重试（NetBSD 秒连）——管理帧路径独立问题。

## 审计定案（本轮根因，全部有 file:line）

修复前 CherryUSB EHCI 对 bulk OUT 的处理：每 URB 新建 QH 且 **`ehci_qh_add_head` 插异步环头**（usb_hc_ehci.c），多笔在途时控制器**逆序/交错服务**；usbdi shim 的 pending 链也是头插、无排序。NetBSD usbdi 每管道 FIFO 严格有序，88E 固件按 bulk OUT 到达序出帧 → 环乱序直接映射为空口帧序混乱（AP 重放窗丢帧），与"USB 全 ACK、air 数据断续、TCP 校验全对但吞吐坍缩"的症状链吻合。

修复形态（C1）：
- `ehci_bulk_urb_gate()`：submit 时按 `urb->ep` 认领端点门闩（无在环 QH → ARM；有 → PARK 入 per-endpoint FIFO，深度 8）。
- `ehci_bulk_ep_release()`：QH 退役（waitup/kill 两路）时弹队首补位，**toggle 从退役 URB 携带**（OUT=arm 时预推进的终值、IN=qTD 回读、HALT=0）——即 NetBSD 持久 QH overlay 的语义等价物。
- parked URB 被 kill（hcpriv==NULL）→ 从 FIFO 摘除并以 -USB_ERR_SHUTDOWN 完成。

## 板测证据

**第 1 步 N6 复跑（netbsd-mfs，上午）**——环境基线：

| cell | 结果 | 并发 ping |
|---|---|---|
| TCP up 30s | **13.8 Mbit/s，0 重传** | 120/120 零丢 |
| TCP down 30s | 14.0/13.9 Mbit/s | 120/120 |
| UDP 15s @30M | 19.4 Mbit/s，0% 丢 | 60.8% 丢（bufferbloat 已知） |

一次 `urtwn0: transmit failed, TIMEOUT` 后自愈。⇒ 环境/ dongle/固件洗清，差量锁定 host USB 层。

**R1（镜像 f9428cce：C1 生效、C2/C3-shim/C4 不在、窗口 8×MSS）**：
- 关联第 3 次成功 → ping 通 → `iperf3 192.168.0.18 30`：**30/30 个 interval 全部输出**（0.09→2-2.5M 平稳），`iperf3 done: total 2.03 Mbits/sec (7647232 bytes)`，**正常收尾**（对照修复前：interval 停印、静默 90s+、worker 卡 send 需强制断开）。
- 关掉 gmac0 有线口重跑：1.96 Mbit/s 同形态（排除有线口分流；也解释 usbstats 全 0 与流量并存的疑点之一）。

**R2（镜像 1c70e9f9：C1+C2+C3+C4+窗口 32×MSS）**：
- 关联第 4 次成功 → `iperf3 30`：**2.25 Mbit/s（8458240 bytes）**，同样全程平稳、正常收尾。
- 窗口 ×4 无效 → **BDP 假设排除**（8×MSS@40ms≈2.3M 的数字巧合是假象；若是 BDP，×4 窗口应接近 ×4 吞吐）。

**第二次 NetBSD 对照（下午，R1/R2 之间）**：TCP up **14.3 Mbit/s**（RTT avg 42ms，120/120 零丢）→ 环境无漂移，2M 限制在我方栈。

**UDP/-r 判别未完成**：嵌入式 iperf3 的 `-u`（cookie ack 失败）与 `-r`（reverse 启动后挂起）客户端路径不可用（既有局限，非本轮回归）。"C1 每帧往返 vs lwIP cadence"的区分留待 usbstats 修复后用完成节奏判别。

## 归因矩阵（本轮新增行）

| 配置 | C1 FIFO | 窗口 | 30s TCP up 结果 |
|---|---|---|---|
| 修复前（v11/v28 多轮） | 无（环头插乱序） | 8×MSS | 突发后静默楔死，~1MB/30s，需强制断开 |
| R1 | **有** | 8×MSS | **2.03M 平稳 30 interval，正常收尾** |
| R2 | 有 | 32×MSS | **2.25M 平稳，正常收尾** |
| NetBSD 同期 | 持久 QH | 大 | 14.3M（上午 13.8M） |

## 下轮清单

1. 修 `wlan usbstats` 零读数（双适配器 dispatch）；用它读 tx 完成节奏 → 区分"C1 arm 往返（若 ~166 完成/s 且成簇=TCP 突发）vs lwIP cadence"。
2. 关联重试问题：抓 RTOS 侧 auth 帧发送/重试节奏 vs NetBSD（`wlan trace 1` 已可用，本轮未见 urb submit 打印，需查 trace 生效条件）。
3. 速率上限若定位到 C1 arm 往返：补位 arm 从 IAA 中断挪到完成扫描同窗口（已在做）或允许同端点多 QH 但按序插入环（折中形态）。
4. 嵌入式 iperf3 的 -u/-r 支持或改用外部打流工具补齐 UDP/下行口径。
5. M7+ 全阶梯（120s、×2 冷启动）在速率课题收口后补跑。

## 备注

- TFTP 部署名 `freertos.bin`（非 `threadx-smp.bin`），staging 后必须 cp 覆盖部署名并 sha256 对拍（本轮第一次 boot 因此跑了旧镜像）。
- rtwdbg.sh 全流程可复用（wifi-debug-deliver.sh → ftp → tar → `IF=urtwn0 STAT=none sh /tmp/nb/rtwdbg.sh`）。
- 收尾：power_off + power_status 复核 + board_release。
