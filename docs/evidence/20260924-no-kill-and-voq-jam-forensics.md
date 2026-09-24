# 停杀 armed bulk 上板 + 88E VOQ jam 取证（满载 wedge 收敛到固件排放路径）

日期：2026-09-24（晚）　分支：wip/urtwn-usb（ea270c5）/ net_80211 a22f439
前序：`20260924-fullrate-crash-fixed-device-wedge-next.md`

## 本轮变更

`usbd_watchdog_sweep` 停杀 armed bulk（收集条件收窄为 `in_xq`，删 armed kill 分支）。
机制依据（代码级核实）：bulk OUT urb 在 **arm 时**就把 `urb->data_toggle` 预推进为
"全长完成后的承诺值"，完成路径对所有状态（含 CANCELLED）无条件回写
`pipe->data_toggle`——设备只消费 k'<k 个包时，k−k' 奇数即永久失步（bulk OUT 无任何
重同步点）。不杀则 qTD 链留在环上原地重试，HCD 无超时、NAK 非错误，设备醒来后按
原 DT 序列继续消费、正确完成。teardown kill 全保留。

## 板验新事实（第 1 轮 30s 上行，kill=0 生效）

- **wd_timeouts=0、kill=0**：新行为生效，零杀。
- **发作提前到 ~1s**（前几轮 25-30s）：0-1s interval 15.00M 后 TX 停摆，
  parked 超时（io timeout）无限循环，**40s+ 无自愈**。
- `usbstats`：`submit_fail=0 guard_drop=0 post_drop=0 kill=0 wd_timeouts=0`；
  `rx_submit=8104 rx_complete=8103`——**RX（bulk IN）全程活着**，只有 TX 死。
- **ep0 活着**：wedge 后 `wlan reg read 0` 秒回 `0xfc178bfc`。
- **设备 TX 队列寄存器转储（`wlan reg txq`，wedge 态）**：
  - `TXPAUSE [0x522] = 0x00`（固件未主动暂停）
  - `VOQ_INFO [0x400] = 0x02ff00ff`，VIQ/BEQ/BKQ/MGQ/HGQ 均为空闲形态
    `0x00ff00ff` —— **VOQ 卡了 ~0x200=512 个包不排放**
  - `TXDMA_STATUS [0x210] = 0x00000401`
- 结论：**88E 固件 OTA 排放路径对 VOQ 内部 jam**（512 包积压、OUT 端点冻结、
  RX/ep0 完全正常、TXPAUSE 未置位）。宿主侧已无可动作（无杀、无失步、三桶全零）。

## 结论修订

1. **kill toggle 失步 = 永久化放大器（已根除），不是 wedge 本体**：无 kill 后设备
   依然冻结且不自愈——wedge 是设备固件 OUT/VOQ 排放路径的内部故障。
2. 发作时刻随机（1s vs 25-30s），非字节量阈值（1s×15M≈15MB vs 25s×13M≈45MB）；
   限速判别（iperf3 -b）不可行——嵌入式 iperf3 无 -b，后续需在 NetBSD 侧做或
   driver 层限节奏。
3. 满载 ~100Hz ep0 vendor-request 洪泛（前轮证据 2）仍是宿主与 NetBSD 的已知行为
   差异候选——NetBSD 同驱动 30s@12.2M 无 wedge；**下一步第一优先：定位满载期间
   谁在持续发 ep0 请求**（urtwn 数据路径 verbatim NetBSD 不做寄存器 I/O，嫌疑在
   port 适配层的周期任务：calib/led/RA/latency 采样类）。

## 下一步（按优先级）

1. **定位满载期 ep0 洪泛源**并消除（若属 port 适配层任务，净移除即回归 NetBSD
   行为形态）→ 复测 30s×2；若 wedge 消失 = 根因闭环。
2. 若仍 wedge：**NetBSD 侧限速/长 soak 判别**（-b 12M、120-300s，检验速率压力 vs
   概率发作）。
3. 若 NetBSD 限速仍 wedge：硬件判别（换电口 hub / 根端口；dongle 温度观察）。
4. 供电侧旁证：`power_status` 板级功耗在 wedge 前后有无阶跃（本轮未取）。

## 提交

- net_80211（feat-rtw8189f-sdio）：a22f439（sweep 停杀 armed bulk）
- fwc-urtwn（wip/urtwn-usb）：ea270c5（镜像快照，含 SDIO 线 rtw88 b529198 同步）
- 镜像：`threadx-smp.bin` 1011704 B，sha256 `7e92c88e…`
- 板卡：power_off 复核 + release（收尾同前）
