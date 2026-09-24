# 满载 crash 已修复（EHCI 生命周期 + usbdi 成员标志）；剩余一阶停摆定位于设备侧

日期：2026-09-24　分支：wip/urtwn-usb（net_80211 `feat-rtw8189f-sdio` 同步）
前序：`20260923-fullrate-attribution-progress.md`（IRQ 补锁证伪 + 证据链）

## 结论速览

| 项 | 状态 |
|---|---|
| 满载 fatal（三趟同值 FAR 0xd51be00058008c0 / b27b00d0） | **已修复**：两趟满载 30s + watchdog 连杀 12 次，**零 fatal、零楔死、系统全程存活** |
| kill/IAA 生命周期毒化（上轮"补锁无效"反常的答案） | **根因闭环**：`usb_hc_ehci.c` 四处修复（5bb96bf） |
| usbdi `:769` walk-off fatal | **根因闭环**：shim 成员标志（net_80211 a427377 / 镜像 bba99f9） |
| 阶梯（3× 冷启动 30s 双向 ≥10M 零楔死） | **未过**：上行 0–25s 满速 12–15.5M，~25s 起 88E 设备侧 bulk 双向聋化 |
| RX 双缓冲（1→4）作为 wedge 假设 | **证伪**（ca5e362/1d90b82，保留：确实消除重投空窗压力） |

## 修复 1：cherryusb EHCI kill/iaad 旗标生命周期（5bb96bf，可上游化）

静态审计闭环的机理（解释了"IRQ 补锁后 fatal 原样复现"——坏状态在遍历前已存在）：

1. **毒源**：`ehci_urb_waitup` `killed==1` 分支置 `killed=2` 后提前 return，
   `remove_in_iaad` 不清；`ehci_qh_free` 也不清 → 走完 kill 握手的槽带毒释放。
2. **引爆器**：IAA 池遍历只判 flag、不验 `inuse`/`urb` → 每次后续 IAA 对毒槽重放
   waitup：对已重绑槽偷跑完成路径（sem 多给→控制传输假完成），对已释放 urb 解引用
   → `sem_give(0xd51be00058008c0)` fatal（槽位复用历史固定 → 确定性同值）。
3. **修复四处**：① killed==1 分支补清 flag；② IAA 遍历加 `inuse && urb` 前置；
   ③ `ehci_qh_alloc` 把 flag 清理挪进 inuse 认领临界区并补 `killed=0`（残留 1 会吞
   新租户首个完成）；④ kill 自旋超时不弃置：槽仍属本 urb 则 kill 补完成
   （free+callback，IRQ 同语义），已竞态完成则静默返回。

## 修复 2：usbdi shim 对非成员 SLIST_REMOVE 的 walk-off（net_80211 a427377）

`:769` fatal（FAR b27b00d0 同值复现）的机理：管道卡死后后续 TX 停入 xq（5s 死线），
sweep 的 **parked 超时分支自己摘链后投递**，而 `usbd_shim_urb_work` 无条件执行
`SLIST_REMOVE(pending)`——对从未 arm（不在 pending 上）的 xfer 做摘链，NetBSD 宏
walk-off 读堆直到撞上 page fault 或碰巧"找到"（随机改写 `curelm->next`，弯折列表
+ 污染分配器字）。时间线完全吻合：stall 起点后 5s parked 死线齐爆 → sweep 先投
parked 后投 kill → worker FIFO 先处理 parked → 崩。

修复：`on_pending`/`on_wd` 成员标志，设于插入点、清于摘链点，urb_work 只摘自己仍
在的链；`in_flight_cnt` 递减移入 on_wd（parked 超时不再下溢）。xq_flush 与
destroy_xfer 防御分支同被覆盖。

## 板验证据（2026-09-24，两趟满载 30s 上行）

**趟 1（5bb96bf + a427377 镜像）**：0–25s 满速 12–15.5 Mbit/s；kill#1 = ep0_urb
0xa286bc8 500ms 超时（t≈+15s，握手干净）；~+25s 起连环 watchdog 杀 TX xfer
（kill#2–#12），**全部握手干净、无 fatal**；收尾 `wlan usbstats`：
`submit=47270 submit_fail=0 complete=47239 guard_drop=0 post_ok=47269 post_drop=0
worker=47269 kill=12`、`q_timeout=30`（30 次 parked 超时全部干净回收，
`tx_complete 27568 = tx_submit 27538 + 30` 账目吻合）、`ctrl_fail=0 ctrl_retry=1
stall_clear=0`。

**趟 2（+RX 1→4）**：kill#1/#2 仍是 ep0 500ms 超时（t≈+5.6s、+23.2s），kill#3
bulk 卡死依旧 ~+29s —— **RX 双缓冲证伪为 wedge 主因**。

## 剩余一阶问题定性（下一轮入口）

满载 ~25s 后 **RTL8188EUS 设备侧聋化**：bulk IN/OUT 双向停止完成（唯一的 RX xfer
常驻在飞不返回），**ep0/控制通道存活**（寄存器 I/O 正常），ep0 也间歇 500ms 无响应
（每趟 1–2 次）。宿主侧三桶全零、kill 全净——无可修之处。

- **现象**：88E 固件在持续 ~14M 下间歇停止服务 bulk 端点（含 ep0 偶发聋），一旦
  bulk 聋 + watchdog kill 触发，**永久化**。
- **永久化的宿主侧放大器（已定位未修）**：kill 把 CANCELLED 完成回写
  `pipe->data_toggle = urb.data_toggle`（usbdi_compat `usbd_shim_urb_complete`），
  而被杀 urb 的线速 toggle 已部分前进 → pipe 重播种为起始值 → 与设备失步 → 设备
  丢弃后续所有包 → 永久卡死。**候选恢复**：kill 后对 TX 管道走 clear-halt
  （shim 已有 `usbd_pipe_clear_halt` 机制，stall_clear=0 说明从未触发）。
- **设备侧候选**：固件 bulk 服务 wedge（供电/温升未排查——dongle 挂 CH334P 总线供电
  hub；NetBSD N6 包络 12.2/12.1M 的持续时长口径待查，可作硬件能力对照）。

## 提交与构建

- fwc-urtwn（wip/urtwn-usb）：5bb96bf（EHCI 四处修复+临时插桩）→ bba99f9（镜像
  a427377）→ 1d90b82（镜像 ca5e362，RX 1→4）→ 6ddd1c6（撤插桩，净修复态）。
- net_80211（feat-rtw8189f-sdio）：a427377（usbdi 成员标志）、ca5e362（RX 1→4）。
- 净修复态镜像：`threadx-smp.bin` 1011688 字节（6ddd1c6 构建）。
- 板卡纪律：借板（锁空闲直接获取），两次冷启动 KI-001 完整流程，收尾 power_off。

## 阶梯口径声明

本轮未达 3×≥10M 双向零楔死：上行 30s 内 0–25s 满速、末段设备侧退化（趟 1 30s
均值仍 ≈11–12M，趟 2 相似），下行 leg 因设备退化未单独成测。**crash 目标达成，
满速稳态目标移交下一轮**（入口见上）。
