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
