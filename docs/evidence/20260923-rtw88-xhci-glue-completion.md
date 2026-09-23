# 证据 2026-09-23 — rtw88(8821CU) xHCI 胶水补全：四个根因闭环，阶梯推进至固件下载

分支 `wip/rtw88-usb`（fwc-rtw88 worktree）：`98db2b7 → 4524b2d → 2ee1145`。
net_80211 仓 `feat-rtw8189f-sdio`：`8505748`（shim 双重摘链修复，fwc 侧镜像同修）。
构建门禁：`make all` / `make freertos` / `make gates` / cleanroom-scan 全绿（4524b2d 与 2ee1145 均过）。

## 板验定界的四个根因（全部有串口证据）

1. **ep0 死 TD 级联**（4524b2d 修复）：ctrl 超时后 TD 原地留环，后续控制传输排在死 TD
   后永久超时（shim 3 次重试堆 4 个死 TD），`usbh_kill_urb` 空操作使 watchdog/modeswitch
   回收失效，STALL 后无任何恢复命令。修复 = per-EP urb 闸门（hcpriv 串联 wait 链，完成
   pop 下一个）+ Stop/Reset EP + Set TR Dequeue 三件套 + kill 实装（恰一次 -SHUTDOWN）
   + 事件环原子单消费者守卫 + 断开时闸门排水。日志证据：每次超时后恢复把环清回 idx=0，
   重试从 idx=0 干净开始。

2. **SET_ADDRESS 劫持 bRequest=5**（2ee1145 修复，"mac power on failed" 真因）：
   RTL8821CU vendor 寄存器访问 `RTW_USB_CMD_REQ = 0x05` 与 USB SET_ADDRESS 同值，
   胶水只按 bRequest 拦截 → 驱动每一笔寄存器读写都被替换成 Address Device 命令
   （日志：attach 窗口 6 次 "assigned usb addr" 刷屏），写从未上线、读恒 0。
   修复 = 拦截条件加 `bmRequestType == 0 && wLength == 0`。
   修复后板验：power-on 全程 600+ 笔 ep0 传输 6ms/笔全 SUCCESS（23.1s–25.1s）。

3. **环回卷 TD 跨 LINK**（2ee1145 修复）：TD 恰跨 254/255/0 时 LINK 落在 TD 中间
   （NetBSD xhci.c 明言此类对齐的 LINK 在 Ivy Bridge 实证失败、ASMedia 类锁死），
   本 DWC3 同症：5 秒无任何事件、Stop EP 停在 idx=0。修复 = `xhci_ring_put` 检测
   批量跨回卷返回 -2，submit/ctrl 路径（线程上下文）先环复位恢复再从 0 重装。
   板验：idx=252 自动触发 → 复位 → idx=0 重装成功。

4. **shim submit 失败双重 SLIST_REMOVE**（net_80211 8505748 + fwc 镜像同修）：
   失败路径摘 `in_flight_xfers` 后 worker 公共路径再摘一次（NetBSD SLIST_REMOVE 对
   不在表内元素是 UB）。改为失败路径不摘链，worker 统一摘两条。

## 阶梯当前位置（boot-212853-35df，2026-09-23）

```
上电冷启 ✓ → bus2 枚举 0bda:1a2b ✓（一次过）→ modeswitch 弹出（CBW 31B + CSW 13B 一次过）✓
→ 设备消失/slot 释放 ✓ → c820 一次枚举 ✓ → rtw88u 认领（3 tx pipe, rx 0x84, int 0x87）✓
→ mac power on 全序 ✓（600+ 笔寄存器访问 6ms/笔全通）→ efuse/chip param ✓
→ 固件下载开始（REG_DDMA_CH0SA 0x1200 写成功）✗ ← 当前卡点
```

## 卡点现象（下一轮入口）

0x1200（DDMA CH0SA）写成功后，0x1204（CH0DA）起设备永久静默；随后 DWC3 的
Stop EP（type=15）/Set TR Dequeue（type=16）命令本身也 5 秒超时——控制器对
slot 1 整体失聪。疑似固件下载触发设备侧 USB 自复位/总线毛刺，把端点和命令环
拖挂。可对照方向：(a) NetBSD/Linux rtw88-usb 的 DLFW 时序与设备自复位等待；
(b) DDMA 参数对设备内存布局的正确性（此前 600 笔读写数据通路已证明无损坏）；
(c) xHC 失聪后的 USBSTS(HSE) 检查与控制器级恢复。

## 附带改进

- 取证日志 `XHCI_EVENT_DEBUG`（0/1/2，=2 时 ep0 全事件 + arm/timeout 打点，
  带 busid）；addr 打印限流。定位后应归 0/1。
- `XHCI_EVENT_DEBUG=2` 下 6ms/笔的 ep0 全事件跟踪是本轮定位的主要工具，留档。

## 状态口径

- 今日实测：modeswitch/枚举/附着/power-on 全链路（多轮冷启动复现）
- 未验证：iperf（未到达）；固件下载及之后全部阶梯

---

# 续轮 2026-09-23（晚）：死亡窗口根因闭环 = 环回卷丢 STATUS TRB（设备无辜）

## 取证链（boot-231647 起四轮冷启动）

1. `xhci_do_command` 超时 dump（本轮新增）显示 USBSTS=0 干净、CRR 卡死 →
   控制器健康、命令单元真停，排除 HSE/事件环自锁。
2. 死亡窗 ep0 全事件：SA 写 TD 恰在 252..254 收尾（ep=255），DA 写批从
   LINK 槽起批 → `xhci_ring_put` 的 LINK 写**吃掉一个载荷迭代**，STATUS
   TRB 没上环 → 设备收 SETUP+DATA 后等 STATUS 永久挂起（假死）、主机 5s
   超时、Stop EP 等 TD 退役挂死、CA 退不了、命令单元整体死亡。
   **上轮"设备失聪/DDMA 页写楔死"结论全部推翻：设备从头到尾无辜。**
3. 连带修复：命令环误用 -2 回卷策略（回卷被当 NOMEM，645s 撞墙）→
   命令环改自然回卷 + 软件 dequeue 随完成推进（此前从不推进，~253 条
   命令后空间核算永久判满）；pipe_recover 的 SET_TR_DEQ 失败改为上抛，
   不再对硬件未重指的环武装 TD；do_command 并发上下文收敛到调用者栈。

## 阶梯新位置（boot-005217，DEBUG=1）

```
fw download + validate ✓（连续多轮）→ RTL8821CU ready ✓ → chip param ✓
→ wpa start：RX 信标流 ✓（rx_complete 数千，status=0 帧长 857/513/473，
  32768=RX 聚合整缓冲）→ wpa connect QiQiJia：scan timed out ×2
→ [新卡点] dci=9 Transfer Event code=26 (STOPPED_LENGTH_INVALID) 后
  wlan worker 数据异常 fatal（FAR=0xaa0100c0）→ H2C 管道缓冲耗尽
```

## 下一轮入口（未修）

- code=26 落在 kill/Stop-EP 完成路径：`xhci_complete_td` 对 stopped 类
  事件与 shim worker 的交接存在 use-after/wild pointer（worker 栈回溯
  ELR=0x0a0a63dc）。对照 usbh_kill_urb 的恰一次归还与 code=26 的 TD 配对。
- 修通后预期：wpa connect 能扫到 QiQiJia（RX 已活），走认证/关联/4 次
  握手 → DHCP → ping → iperf 600s×2 + 补 2 轮冷启动。

## 提交

- net_80211 `feat-rtw8189f-sdio` `cf067ea`：vendor 寄存器读写失败可见化。
- fwc-rtw88 `wip/rtw88-usb` `d43c543`（cmd ctx + 取证 + 镜像）、
  `2d9c8b6`（环回卷 STATUS 丢失修复）。XHCI_EVENT_DEBUG 已归 1。

---

# 续轮 2（2026-09-24 凌晨）：code=26 崩溃修复 + RX 证据链闭合，堆踩踏实锤

## 已修并板验

1. **complete_td 陈旧槽二次归还**（e5d7f95）：多 TRB TD（RX 64-TRB 聚合）完成时
   只清事件落点槽的 urb，恢复路径的 Stopped 类迟到事件落在其余槽 → 同一 urb
   二次归还 → shim worker 二次 SLIST_REMOVE 数据异常（上轮 fatal 的真因）。
   修 = 入口守卫（urb==NULL/state!=0 丢弃）+ 完成时整段清 urb。
2. rxeof 两个静默丢弃门加了计数+限流打印；net80211 icstats/lwip bridge 计数
   （`wlan stats`）；`wlan dbg <mask>` 运行时开 rtw88 调试位；workqueue NULL
   func 取证守卫（net_80211 9734995）。

## RX 证据链（boot-020120，binary 5b1b9f7e）

- USB 层 RX 完成数千笔、零 submit_fail；net80211 层 rx=0、扫描表空（除一条
  全零占位节点）→ 帧死在 rxeof 两个静默门之间。
- **主导流 = `rx drop len=32768`**：设备把 32K 缓冲聚满才 flush（短完成缺失）
  → Linux 语义整缓冲必丢 → 已改为放行进 demux。
- **`rx drop status=1`**（数百级）：第二类错误完成，error code 待定位。
- **TLSF heap assert 复现两轮**（tlsf.c:462 block_next !block_is_last）→
  堆踩踏实锤，与 NULL func workqueue fatal 同源。头号嫌疑：聚合满 32K 时
  帧 DMA 越过缓冲边界（设备聚合行为 ≠ 驱动配置的 agg off）。

## 下一轮入口（未修）

1. **RX 聚合配置为何不生效**：`dynamic_rx_agg(false)` 写 0x280=0x0100 +
   0x10C BIT2 每 2s（watchdog）都在写，设备仍聚满 32K 才 flush。对拍
   NetBSD 侧同寄存器行为（board-proven 路径、同 dongle）找语义差/丢写。
2. **堆踩踏定位**：满缓冲 DMA 越界假象成立的话，修复 1 即消失；否则给
   RX 缓冲区加 canary 定位踩踏者。
3. 堆修好 + RX 短包正常后：scan 应能看到节点 → wpa connect → DHCP →
   ping → iperf 600s×2 + 补 2 轮冷启动。

## 提交

net_80211 `1381aba`+`0cb547c`（rxeof 取证/整缓冲放行）、`9734995`（workqueue
守卫）；fwc-rtw88 `e5d7f95`（complete_td 守卫+整段清 + 镜像 + dbg 命令）。
XHCI_EVENT_DEBUG=1。

---

# 续轮 3（2026-09-24）：demux 崩溃定位 —— RX 帧死因 = worker 线程被踩死

## 新证据（boot-022048，binary d23a6d2d，含 shim err 打印 + demux 入口计数）

- `rx work run 0/1, skbq 1`：**demux 确实在跑、队列确实有帧**（整缓冲放行
  生效，skb 链进了 demux）。
- `rx work run 1` 之后即刻 `fatal core0 ESR=96000004 FAR=b27b00d0
  ELR=0a0a69a4`（线程 wlan-wor）→ addr2line = `usbd_shim_urb_work:795`
  的 `wlan_usb_latency_record`，处理了一个 magic 恰好合法但内容已被重用
  的 xfer。worker 死 → RX 从此全断（net80211 rx=0、扫描表空）。
- `wlan dbg` 命令实测 bug：打印 `rtw_debug_mask=0x0`（strtoul/csh 参数
  传递问题待查），本轮 demux 丢弃打印不可见。
- `rx drop status=1`（=USBD_IN_PROGRESS）类仍在：这些 xfer 的 status 从
  未被完成路径设置，来源待查（shim 已加原始错误码打印 `shim: rx
  complete err=`，本轮未及捕获）。

## 因果链（当前最佳解释）

设备聚合满 32K 才 flush → 整缓冲进 demux → demux/latency-record 路径
存在对被释放/被踩内存的访问（TLSF assert 两轮 + FAR=b27b00d0 两轮同值
域）→ worker 死 → RX 静默。堆踩踏的第一现场仍未定位（候选：满缓冲 DMA
边界 / rtw88_skb 生命周期 / latency record 环）。

## 下一轮

1. 修 `wlan dbg` 参数解析（argv 偏移或 strtoul base）。
2. demux 崩溃：在 `usbd_shim_urb_work` 入口对 magic+指针域做二次校验，
   崩溃点前 dump xfer 邻域；排查 `wlan_usb_latency_record` 对异常 xfer
   的字段访问（先把它短路再放行 demux，二分定位）。
3. rxeof status 门打印已带 err 码（`shim: rx complete err=`），下一轮
   直接抓取分类。
4. 堆修好 → scan 节点表 → wpa connect → DHCP → iperf。

## 提交

net_80211 `19ff73f`+`9257cc1`；fwc-rtw88 镜像 + worker fatal 定位记录
（本节）。binary d23a6d2d 停在 fatal 后现场（未再复跑）。

---

# 续轮 4（2026-09-24）：demux fatal 修复 + shim err 码取证 + 1024 环回归回退

- complete_td 守卫+整段清（e5d7f95）板验站稳：boot-023812 上 demux 768 次
  运行、skbq 持续有帧、无 fatal（此前 run 1-2 即死）。
- shim 完成点取证（16cfd01，net_80211 9257cc1 手工镜像）：`shim: rx
  complete err=` 打印原始 cherryusb 错误码；`wlan dbg` 命令本轮确认有
  解析 bug（打印 mask=0x0），待修。
- **镜像事故记录**：3deadb1 误将 net_80211 新版 usbdi_compat.c（+418 行
  刻意分歧变体）覆盖 fwc 刻意保留的旧变体；16cfd01 恢复并手工只打
  rx_err 补丁。教训：fwc 的 usbdi_compat.c / rtw88_compat.c 为刻意分歧
  变体，禁止整文件镜像，只能手工移植补丁。
- **1024 传输环实验失败回退**：环扩到 1024 后 power-on 卡死（ctrl 风暴
  val=04e0/0x30 循环超时 + "chip bring-up did not complete"），疑似堆
  压力，已回退 256；实验记录在案。

## 下一轮（不变 + 新增）

1. `wlan dbg` argv 解析修复（实测 mask 落 0）。
2. `shim: rx complete err=` 抓取分类（本轮已接线未及捕获）。
3. demux 崩溃二分：latency record 短路实验。
4. RX 聚合寄存器对拍 NetBSD（0x280/0x10C 行为差）。
5. 堆修好 → scan 节点 → wpa connect → DHCP → iperf 600s×2。
