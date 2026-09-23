# urtwn 提速 Phase 1 板测 runbook（cherryusb 5019680e + B1 优先级同步后）

前置状态：`wip/urtwn-usb` @ 0e61e87（130f7e3 = cherryusb dev 同步 + kill 握手上游化；
0e61e87 = wlan worker ThreadX 16，B1 同步）。镜像 `build/rk3568-threadx/threadx-smp.bin`
sha256 `75434fbd…`（1010904 B）。对标 N6（NetBSD urtwn0 boot-A）：TCP 上 12.2 / 下 12.1 /
UDP sender 18.3 Mbit/s；本轮基线 R-A：上 2.34M（wip/urtwn-usb 4d5907a 形态）。

## 准备（宿主）

1. `service_status` 全绿（portproxy 指向当前 WSL IP）。
2. `board_acquire {note: "urtwn cadence attribution: usbstats buckets + B1 priority sync"}`。
3. iperf3 服务器（Windows 192.168.0.18）：
   `powershell.exe -NoProfile -Command "Start-Process -FilePath 'D:\Software\iperf3.19.1\iperf3.exe' -ArgumentList '-s' -WindowStyle Hidden"`
   （防火墙已授权；**别用 Downloads 的 3.1.4**——errno 103）。
4. staging：`tftp_stage {paths: [fwc-urtwn/build/rk3568-threadx/threadx-smp.bin]}` →
   **cp 覆盖部署名** `/mnt/d/tftpboot/freertos.bin` → sha256 对拍（源 75434fbd…）。
5. `boot_os {os: "freertos"}`（冷启动 KI-001 自动）。

## 板测序列（cherrysh，串口单行 `\r`、每条 ≥1.2s）

1. 等 shell → `wlan select urtwn`（c8c01fb 起 reg/usbstats 按所选适配器分发）。
2. `wpa connect QiQiJia paopaojie520`（已知 2-7 次关联重试，等 CONNECTED）。
3. DHCP → ping 192.168.0.18 ×4（历史 ~11ms）。
4. **回归读数**：`wlan usbstats` ×2（间隔 1s）——确认 submit/complete 在走、
   tx/rx usbtime 有样本（同步后首次上板，dispatch 修复 + 新 kill 路径首验）。
5. **基线 30s 上行**：`iperf3 192.168.0.18 30` ——记录总速率 + interval 形态
   （对照 R-A 2.34M；B1 修复后若显著抬升 = 优先级是主限，记录新值）。
6. **稳态中途读数**：跑到 ~15s 时在第二终端没法并发，改为跑完后立即
   `wlan usbstats`（快照 1）→ `iperf3 192.168.0.18 30` → 结束后立即
   `wlan usbstats`（快照 2）→ **差值即本轮 queue/hcd/wake 增量**，
   除以 `usbtime tx n` 增量 = 每帧三桶均值。
   判读：6ms/帧 落 wake → worker/semaphore；落 hcd → EHCI/88E 节奏；
   三桶都小而速率仍低 → 瓶颈在 lwIP/TCP 侧（看 INPKT 丢包与 ACK 节奏）。
7. **mbox 丢包 signature**（B1 验证）：shell `net` 统计（或 lwIP stats）看
   TCPIP_MSG_INPKT err 是否为 0（SDIO 线在 ThreadX 5 时 600s 积 1799）。
8. 若时间允许：`iperf3 192.168.0.18 30 -R` 试下行现状（预期复现 SDIO 线的
   lwIP RX 窗口/ACK 停摆形态——UDP 下行 PASS / TCP 下行 4s-6min 停）。

## 收尾

`board_release` → `power_off {confirm:true}` → `power_status` 复核。
证据（usbstats 快照 + iperf interval）落本目录新 evidence 文档。

## 判读后的下一步（Phase 2）

- wake 桶大 → 完成路径瘦身/优先级复查（B1 已落，看剩余）。
- hcd 桶大 → 审计 arm→done：QH 分配/插环/doorbell 开销、88E bulk OUT 固件节奏
  （usbstats toggle 节奏可判；对照 NetBSD ~1ms/帧 → 固件不是下限）。
- 都小 → lwIP/TCP：ACK 节奏、TCP_WND 32*MSS 实效、iperf3 send 循环；
  下行走 SDIO 线已收敛的 RX 窗口/ACK 路径方向。
