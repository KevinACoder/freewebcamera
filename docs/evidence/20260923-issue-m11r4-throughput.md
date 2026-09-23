# ISSUE: M11 r4 — SDIO WiFi (RTL8189FTV) 吞吐天花板（性能优化交接件）

状态：**open**（移交性能优化）｜ 记录人：M11 r4 轮（2026-09-23）
稳定基线镜像：`3175756`（build `706e52d0…` 部署 `/mnt/d/tftpboot/freertos.bin`）；
issue 文档所在分支代码含 B2b 窗口改动（见文末提交清单）。

## 1. 现状（今日实测，600s 级 iperf3 vs Windows 192.168.0.18:5201）

| 方向 | 结果 | 说明 |
|---|---|---|
| TCP 下行 600s | **1.10 Mbit/s**（83.1 MB），零停摆 | B2b 镜像（8*MSS 窗口 + SDIO 中断化） |
| TCP 上行 600s | **2.69 Mbit/s**（202.1 MB），零停摆；整程区间 2.0–3.3 | 同镜像；过 B1 的 155s 崩溃关口 |
| UDP 下行 30s | 2.61 Mbit/s 均值 / 5.84 峰（r3 记录） | |
| 参照 | Linux rtl8189fs 同卡 12.1/11.2 M（HT40）；NetBSD USB 卡同环境 12–14 M；NetBSD SDIO 同驱动 6M 速率帽 UDP 60s 仅 4.6 M | RTOS 侧约为可用包络的 1/10 |

## 2. 本轮已排除的假设（重要——不要重复实验）

| 假设 | 实验 | 结果 |
|---|---|---|
| tcpip mbox 丢包 | 深度 16→64 + INPKT 池 16→64（提交 0f7905e） | INPKT err 1799→**0**（max 12/64），吞吐不变 → 不是天花板 |
| 优先级倒挂加剧丢包 | wlan-work ThreadX raw 5→16（低于 tcpip 15） | INPKT err 归零；暴露了独立 SMP 竞态（见 §3） |
| poll 延迟（10ms drain） | SDIO DAT1 卡中断化全链（1bedfd7/69e40c1） | RTT min 9→7ms；吞吐不变 → 不是主因 |
| TCP 窗口 BDP | TCP_WND 4→8*MSS | 1.42→1.10（同量级波动）→ 窗口不是瓶颈 |
| lwIP memp/mem 8 对齐、PBUF 池不足 | lwstats 全池 err=0，HEAP max 9.5K/128K | 排除 |

## 3. 本轮顺带修掉的三个真 bug（已收口，勿回退）

1. **SMP kernel-lock 竞态**（`cmsis_os2_impl.c`，提交 3bcd465）：`kernel_lock_count`/`kernel_lock_daif[]`
   全局共享非原子，两核同时 osKernelLock 时计数互踩，输家的 unlock 会放掉赢家持有的
   `_tx_thread_smp_protect()` 全核锁 → 第三核闯入 lwIP 内存临界区。板证：600s 上行 155s 处
   两核 fatal 于 `do_memp_malloc_pool`（memp.c 259/268），自由链表头被读成 0x2（块双分配签名）。
   修法：状态按 MPIDR Aff1 每核独立。
2. **ISR 污染 poll 数据路径**（`drivers/dwc_mmc.c`，提交 69e40c1）：arm INTMASK bit16 后，
   为 IRQ 传输模式写的 ISR 会读到进行中 CMD53 的 IDMAC RI（先于控制器 DTO 置位）或 poll 循环
   即将消费的 CDONE/DTO → 发假完成事件（尾部停 DMA）+ W1C 抢走 poll 等待的位 → 全部 CMD53 超时。
   修法：INTMASK 恰等于 SDIO_INT（poll+中断形态）时，ISR 只处理 bit16：自掩码 → upcall →
   单 bit W1C。
3. **spurious 刷屏**（`tx_glue.c`，提交 3175756）：电平源在 ISR 内自掩码后，线落传播跨时钟域
   慢于 EOI → re-pend 蒸发 → 下一次 IAR=1023。每次卡中断一条、无害，打印节流为 x100 计数。

## 4. 剩余瓶颈候选（按预期收益排序 —— codex 的工作清单）

### C1. TX 全部帧走 QSEL_MGNT + 固定 6Mbps（最高优先）
- 位置：`third-party/net80211/driver/rtw8189f/rtw8189f_chip.c` tx_frame 内
  `RTW8189F_TXDESC_QSEL_MGNT` + `RTW8189F_RATE_6M`（约 :1791-1805；**NetBSD fork 同位置同病**
  ——`rtk3568_lab/os/netbsd` rtw8189f_chip.c:1791-1792 一字不差；这解释 NetBSD N1 UDP 60s
  仅 4.6M ≈ 6M PHY 帽）。
- 机理：所有帧（数据、TCP ACK）都从管理队列以 6Mbps 发出 → 每帧空口时间被放大 ~9 倍；
  下行流中板子发的 ACK 也吃 6M 空口时间，直接拖累下行吞吐（上行 2.4-2.9 > 下行 1.1-1.5
  的不对称与此吻合：上行 ACK 由 AP 以高速率发）。
- 改造方向：数据帧迁数据队列（QSEL EDCA/HIQ 系列，对照 vendor 参考与 NetBSD if_rtw8189f
  的页回收路径——FREE_TXPG 轮询已存在于 tx_frame）+ OFDM 54M（或 24/36M 起步）+
  USE_RATE/重试字段核对；管理/广播帧保持原状。
- 验证：上下行 600s + 服务器侧 retransmits + `wlan stats` rx_errors；预期双向数倍。
- 风险：数据队列的固件交互与 TX 页回收节奏（KI-040 同族）；逐字导入区改动按
  NET80211_PORT(L) 标记并登记 imports.md，镜像回 NetBSD。

### C2. SDIO 排空节奏 vs 空口突发 → 芯片 RX FIFO 无形溢出
- 机理：每帧排空 = CMD52 读 RX0_REQ_LEN + CMD53 读数据 ≈ 400-500µs；AP 以 54M 突发
  发一窗（11.7KB ≈ 2ms 空口）时到达速率 > 排空速率，16KB RX FIFO（RX_DMA_SIZE_8188F）
  尾部静默溢出——芯片无计数器，驱动 rx_errors 不动。TCP 表现为丢段→RTO→窗口塌缩→
  慢启动循环，解释每秒速率 0.3–3.2 的剧烈锯齿（pcap 可复证：服务器重传簇）。
- 方向：a) 验证/启用一次 CMD53 读走整个聚合 burst（驱动 drain 循环已支持聚合记录格式
  24B desc + drvinfo；确认 `RTW8189F_RXBUFSZ` stage 与 64 包上限没有把 burst 切碎）；
  b) 减小每帧命令开销（连续排空时跳过重复的 FIFO 状态查询）。
- 验证：ping 锯齿幅度、服务器 retransmits 簇是否消失；`decryptcrc`/`rx_errors` 应保持 0。

### C3. lwIP 桥三拷贝 → 单拷贝
- 现状：DMA→stage→mbuf→pbuf(PBUF_RAM)+pbuf_take，每帧 ~1.5KB 两次额外 memcpy + 堆分配。
- 方向：pbuf_alloc(PBUF_POOL) 池化直拷一次；或 ref 型 pbuf 指向 stage 缓冲 + 生命周期
  交接（风险高，stage 复用要挂到 pbuf free 回调）。
- 收益估计：中等（CPU 侧，非空口）；在 C1/C2 之后再做，避免掩盖主要矛盾。

### C4. 结构性上限（远期）
- net80211 无 HT：11g OFDM 54M PHY → TCP 实际 20-25M 结构帽（Linux 同卡 12M 是 HT40）。
- 走 HT（802.11n）是里程碑级工程，不在本 issue 范围。

### C5. 残留观测谜团（低成本定位项）
- 中断化后 1400B ping min RTT 7ms：10ms poll 已除，仍有 ~5ms 未解释（SDIO 命令间隔、
  serializer 排队、lwIP/驱动处理）。板上打点（tick 级时戳）可定位；不直接换吞吐，但影响
  BDP 换算与 C2 的节奏分析。

## 5. 观测工具（板上现成）

`lwstats`（memp err/INPKT max/heap）、`wlan lwip`（桥五计数 posted/gated/pbuf_fail/
take_fail/input_fail）、`wlan stats`（rx_errors/decryptcrc）、`wlan sdreg`（HIMR/HISR/
REQ_LEN）、spurious x100 节流计数、宿主 `iperf3 -s` 侧 retransmits、Windows pktmon。

## 6. 本轮提交基线（性能工作从此起跑）

- `0f7905e` B1：wlan-work 优先级内核条件化（ThreadX 16/FreeRTOS 5）+ lwIP 邮箱/池加深
  （TCPIP_MBOX 64、INPKT 64、API 32、RECVMBOX 32、MEMP_NUM_PBUF 32、HEAP 128K）
- `3bcd465` kernel-lock 每核化（SMP 竞态修复）
- `1bedfd7` SDIO DAT1 卡中断化全链（dwc_mmc EVT_SDIO_INT / glue / shim / rtw8189f
  HIMR bit0 + worker 中断唤醒，10ms poll 保留为看门狗）
- `69e40c1` ISR poll 守卫
- `3175756` spurious 打印节流
- B2b 窗口（TCP_WND 8*MSS）与 evidence/DESIGN 更新：见本文件同批提交。

## 7. 复现与测量纪律

KI-001 冷启动（断电≥6s/稳定 12s）→ boot_os os:"freertos"（禁 standalone）→ TFTP
staging sha256 对拍 → 串口单行 \r ≥1.2s → `net down 0`（KI-027）+ 宿主 `arp -d *` →
debug=0 → Windows `iperf3.exe -s`（192.168.0.18:5201）→ 600s × 上下行 + ping 20/20
（测中测后）→ lwstats 前后对拍 → 收尾 board_release + power_off{confirm:true} 复核。
