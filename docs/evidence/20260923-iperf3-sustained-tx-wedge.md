# iperf3 持续 TX 坍缩归因 —— RA 钩子净害实锤（默认禁用）+ 满速能力证实，固件版本假设排除

日期：2026-09-23　提交：net_80211 `fecb965`（feat-rtw8189f-sdio）+ freewebcamera `03ef114`（wip/m11-sdio-rtw8189f 镜像）
镜像：`make all` ThreadX SMP，`build/rk3568-threadx/threadx-smp.bin`
sha256 `d496f65303a4f8fae755be10f7e009695bf7967b3085ae7c9d50c4ef35a4cce4`（1008232 B，含 M11 SDIO 线代码），deploy 对拍一致
dongle：RTL8188EUS `20:f4:1b:52:8f:01`（同前几轮）；本镜像下枚举为 urtwn1（M11 的 rtw8189f SDIO 探测占用 unit0 失败，与本轮无关）
前置：freewebcamera `eb2f723` EHCI bulk OUT toggle 软件化已在镜像内

## 结论（TL;DR）

1. **RA 维护钩子是净害，已默认禁用**：v11 固件上关钩子（`wlan ra 0`）后 iperf3 TCP 上行 2-3s 区间即达 **10.51 Mbit/s**（NetBSD N6 同级）——此前所有"涓流 + 秒级楔死"均为钩子错误载荷所致。钩子的 MACID_CFG/RSSI 载荷从未对过 vendor ABI；其前提"88E 不编程 RA 表 → 固件楔死"被 N6 与本轮双重推翻。`wlan_ra_hook_enabled` 初值改 0，保留 `wlan ra 1`/`fwfix` 实验入口（net_80211 `fecb965`）。
2. **满速能力反复证实**：10.51 Mbit/s（v11 关钩子区间值）、7.70 Mbit/s（v28 最后一秒突发）。板卡 + 驱动 + lwIP + USB 栈具备 11g 满速吞吐能力。
3. **残留阻塞 = 持续满速灌入下固件 TX 坍缩**："满速突发 + 秒级停顿"交替，无法维持；**固件版本假设被排除**（Realtek v11 与 v28 两构建同征状）。host USB 层全程零瑕疵（tx 全部完成、wd=0、kill=0、TCP 数据校验全对）。
4. M7+ iperf 验收维持 open。下一轮首选：本板 Linux 线 rtl8xxxu 同 dongle 差分 + CherryUSB 持久 QH 实验。

## 板测 A（v11 固件 + `wlan ra 0` 运行期关钩子，零重建）

- 冷启动 → `wpa start` → `wlan ra 0`（回显 `wlan ra hook enabled=0`）→ connect → CONNECTED（无 88e fw maintenance 打印）→ ping 5/5
- `iperf3 192.168.0.18 30`：1-2s 0.09 → **2-3s 10.51 Mbit/s** → 此后无 interval 输出；串口静默 ~2min（shell 存活，`echo` 回应 command not found）
- `wlan usbstats`：submit=8771 complete=8770（kill 路径补齐），**tx_submit=269 tx_complete=269 rx_submit=8502 rx_complete=8501 kill=0 wd_timeouts=0** guard_drop=0 post_drop=0
- `wlan reg txq` 双读（间隔 ~70s）逐字节静止：FIFOPAGE=0x009b0000、TXPAUSE=0x00、TXDMA_STATUS=0x2421、全队列头尾指针不动
- 停顿后 `ping` 0/4，伴随 `urtwn0: could not send firmware command 5`（H2C 也发不出）

## 板测 B（v28 固件 + 钩子默认关，重建镜像 d496f653）

- 冷启动 → connect → CONNECTED → ping 5/5 → 健康态 `wlan reg txq` 基线：TDECTRL=0x3300aa10（高字节为活动计数器）、TXDMA_STATUS=0x00000401（健康态即此值，前轮 0x2421 是累积特征而非楔死标志）
- `iperf3 192.168.0.18 30`：1-2s 0.09 后无 interval，串口静默 ~92s；`wlan usbstats`：**tx_submit=727 tx_complete=727 wd=0 kill=0** peak_inflight=3
- 再次 `iperf3 … -u` → `iperf3: test already running`（worker 仍阻塞在 send，无 stop 子命令）
- `wpa connect`（触发 DISCONNECT reason=3）→ 阻塞解开，测试收尾：
  `[iperf3] WARN: tcp send error, errno 103` → **29-30 sec 7.70 Mbits/sec** → `iperf3 done: total 0.25 Mbits/sec (974848 bytes in 30 sec)`

## 归因状态矩阵

| 配置 | 固件 | RA 钩子 | 结果 |
|---|---|---|---|
| M7+ 原始 | v11 | 开（DTC=0） | ~1.2MB 后全 ACK 静默（=toggle 丢弃，eb2f723 已修） |
| 第一轮 toggle 修复 | v11 | 开 | 涓流 0.1-0.75M ~10s → bulk OUT 无应答，wd 连杀 22 |
| **本轮 A** | v11 | **关** | **10.51M 满速** → ~1.9MB 后安静坍缩（host 全零瑕疵） |
| **本轮 B** | **v28** | **关（默认）** | 同坍缩形态，974KB/30s，末秒 7.7M 突发 |

⇒ toggle 链（层1）与 RA 钩子（层2a）已闭环；**层2b：持续满速下的固件 TX 坍缩与固件版本无关、与 host USB 计数无关**。疑点收敛到 host 侧 USB 行为差异：CherryUSB 每传输一 QH + IAAD 摘挂（~800 次/s）vs NetBSD 持久 QH；`USBD_FORCE_SHORT_XFER` 的 ZLP 语义在 CherryUSB 路径缺失；ep0 控制传输节奏差。

## 下轮计划

1. **Linux 差分**：本板 Linux 线（rk3568_lab linux-upstream + rtl8xxxu + rtl8188eufw v28）同 dongle 同 AP 跑 iperf3——若 Linux 稳，则 host USB 行为差异定案，按上面疑点清单做 CherryUSB 侧单变量实验（首选持久 QH for bulk OUT）。
2. kill 恢复路径加固（CLEAR_FEATURE(ENDPOINT_HALT) 双侧回 DATA0 / 接通 urtwn re-init）。
3. `wlan reg read 10d8/10dc`（USB_TX_AGG，无 0x 前缀）双栈对拍。

## 备注

- cherrysh 无 `echo` 命令（探活用任意已有命令）；`iperf3` 无 stop 子命令，worker 阻塞时只能等 socket 错误或重连解锁。
- 收尾：power_off + power_status 复核 + board_release 已按规程执行。
