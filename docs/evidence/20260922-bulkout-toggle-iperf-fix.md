# EHCI bulk OUT toggle 软件化板测 —— 失败模式实质改变，USB 层根因证实，暴露 dongle 固件 TX 楔死新层

日期：2026-09-22　提交：freewebcamera `eb2f723`（wip/m7-net80211）
镜像：`make all` ThreadX SMP 主线，`build/rk3568-threadx/threadx-smp.bin`
sha256 `e52e393be384833a1114a93600c68e12b47dbf049cb4eb0384d77488b1ca59d7`，`make deploy` 前后对拍一致（部署名 `freertos.bin`，956400 B）
dongle：RTL8188EUS，MAC `20:f4:1b:52:8f:01`（与 M7+/N6 同一根），bus1 hub3 port1，1 rx pipe + 2 tx pipes

## 结论（TL;DR）

- **bulk OUT toggle 软件化（DTC=1）生效且失败签名完全改变**：修复前所有传输秒完成但数据零上空口（"ACK 但丢弃"）；修复后 **数据真实到达 dongle 并上了空口**（10 s 内约 0.15–0.2 Mbit 累计，突发 0.75 Mbit/s，形态接近 1Mbps 基本速率）——CherryUSB"每传输一 QH + qTD token 读回"的 toggle 软件链确证为真 bug，修复保留不回退。
- **暴露第二层墙**：约 10 s / 累计百余 KB 后 dongle 固件 TX 管道楔死——bulk OUT 端点从此完全不再 ACK/NAK 应答，host 侧每笔 TX 挂满 5 s 被 usbdi watchdog 击杀（wd_timeouts=22 且持续），ping 0/4，H2C 固件命令（command 5）也发不出；RX 侧 beacon 持续流入不受影响。
- iperf3 TCP 上行 30 s 未通过（涓流后饿死）。M7+ 验收维持 open，下一轮方向见文末。

## 改动（`third-party/cherryusb/port/ehci/usb_hc_ehci.c`，提交 eb2f723）

1. `ehci_qh_fill`：QH.DTC=1 条件从"仅 control"扩为"control 或 bulk OUT"。
2. `ehci_bulk_urb_init`：bulk OUT 软件管 toggle——起始值写每枚 qTD token DT 位（bit31），按 ceil(xfer_len/mps) 逐枚推进，函数末尾把最终值写回 `urb->data_toggle`；零长度 qTD 计 1 包。
3. `ehci_check_qh`：成功分支对 bulk OUT 跳过"最后一枚 qTD token 读回回填"（EHCI spec 不要求控制器写回 retired qTD 的 DT 位；DTC=0 下读回恒为写入初值 0）。

bulk IN / intr / control 零改动；usbdi shim 零改动。登记：`docs/imports.md` §1 CherryUSB 行【2026-09-22 追加 2】。

## 板测阶梯（冷启动 KI-001，串口逐条）

| 步骤 | 结果 |
|---|---|
| `wlan usbstats`（基线） | 全零 |
| `wpa start` | if_init done flags=8843，supplicant running |
| `wpa connect QiQiJia paopaojie520` | Associate 14:a3:2f:18:07:2c → PTK/GTK=CCMP → CONNECTED |
| `wlan status` | state=RUN opmode=1 ch=2412，tx=29 txerr=0 rx=538 |
| `net down 0` → `ping 192.168.0.18` | 5/5 全通（7/45/2/10/2 ms） |
| `iperf3 192.168.0.18 30` | **1-2s 0.09 / 3-4s 0.19 / 4-5s 0.75 / 6-7s 0.06 / 7-8s 0.16 / 9-10s 0.06 Mbits/s**，10 s 后 interval 停打（数据饿死），未见总评 |
| 测试后 `wlan usbstats` | submit=8771 complete=8770（kill 路径补齐），tx_submit=269 tx_complete=269，rx_submit=8502 rx_complete=8501，**kill=22 wd_timeouts=22**，guard_drop=0 post_drop=0 task_drop=0 |
| 随后持续 | watchdog 每约 1 s 击杀一笔（`iaad lost, finishing killed urb inline`），持续至断电前（660 s+） |
| `ping 192.168.0.18`（楔死后） | send 4 发 0 回，伴随 `urtwn0: could not send firmware command 5` |

## 现场固定（楔死稳态，两读相隔约 70 s 逐字节一致 = 完全静止）

```
CR [0x100]=0x000206ff  PBP [0x104]=0x00000011  TRXFF_STATUS [0x118]=0x00000000
HMETFR [0x1cc]=0x00000001  RQPN [0x200]=0x008e000d  FIFOPAGE [0x204]=0x009b0000
TDECTRL [0x208]=0x2500aa10  TXDMA_OFFSET_CHK [0x20c]=0x10fd0000
TXDMA_STATUS [0x210]=0x00002421  RQPN_NPQ [0x214]=0x00000d0d
VOQ [0x400]=0x02ff00ff  VIQ [0x404]=0x00ff00ff  BEQ [0x408]=0x00ff00ff
BKQ [0x40c]=0x00ff00ff  MGQ [0x410]=0x00ff00ff  HGQ [0x414]=0x00ff00ff
BCNQ [0x418]=0x0fff00aa  CPU_MGQ [0x41c]=0x000000aa  TXPAUSE [0x522]=0x00
```

要点：页计数不积累（非队列拥塞型）、TXPAUSE=0（固件未关 MAC 阀门）、TXDMA_STATUS 错误位锁存不增长、全部队列头尾指针静止。`wlan reg read 0x10d8`（USB_TX_AGG）因 parse_hex 不吃 "0x" 前缀实际读了 0x0（回显 `reg[0x0000]=0xfc178bfc`），下轮用不带前缀语法重取。

## 解读：签名对比

| | 修复前（M7+，0bbc031） | 修复后（本次，eb2f723） |
|---|---|---|
| TX URB 完成行为 | 全部秒完成 | 前期正常，约 10 s 起 bulk OUT 挂死（设备持续不应答） |
| 空口数据 | 零 | **有（涓流 + 0.75 Mbit/s 突发）** |
| usbstats | 严格配对 wd=0 | 配对但 wd_timeouts=22 且持续增长 |
| ping/控制面 | 正常 | 楔死后 0/4，H2C 失败 |

第一次 watchdog 击杀出现在开测后约 15 s，而涓流在前 10 s——**楔死首因先于任何 kill**，排除"击杀路径 toggle 反推"作为首因（该路径在 kill 后仍会留下 toggle 不可知状态，属待加固项，非本层根因）。

机制链：toggle 修复后 dongle 固件真正收到 bulk OUT 数据 → 固件以接近 1Mbps 基本速率的效率向空口排放 → 排放速率远低于 host 灌入 → dongle USB RX FIFO 满即 NAK（正常流控）→ 但固件排放随后彻底冻结且不再恢复（连 NAK 都停止）→ bulk OUT 永久无应答。**同一驱动 + 同一固件 blob 在 NetBSD N6 上 30 s@12.2 Mbit/s 无一击杀**，故楔死在"我们 host 侧与该固件的交互"里，驱动与固件本身已被 N6 洗清。

## 下轮方向（按优先级）

1. **速率适配/排放速率**：空口形态≈1Mbps 基本速率 → 查 88E 固件 RA 上报链路（C2H 随 bulk IN 携带）是否在我们栈上被正确送达 urtwn；对照 `wlan ra` 输出。
2. **楔死恢复路径**：NetBSD 的 if_watchdog 超时会整机 re-init（重载固件、重开 pipe，双侧 toggle/端点状态归零）；我们端口击杀后仅重排队、无端点复位。加固：kill 路径明确 toggle 语义 + `CLEAR_FEATURE(ENDPOINT_HALT)`（双侧回 DATA0）或接通 urtwn 整机 re-init。
3. **USB_TX_AGG 对照**：`wlan reg read 10d8 / 10dc`（注意无 0x 前缀）与 NetBSD 同寄存器对拍。
4. 备选：Linux rtlwifi `rtl8188efw.bin` 固件 blob 对照实验（若 1–3 不能闭环）。

## 遗留与备注

- PC 侧 `iperf3 -s` 控制台本轮未抓取（板侧 interval + dongle 寄存器已足够定性）；下轮长测建议同录。
- 击杀稳态下串口被 watchdog 行刷屏，交互命令回显可能夹杂在击杀行之间。
- 收尾：`power_off` + `power_status` 复核已按规程执行。
