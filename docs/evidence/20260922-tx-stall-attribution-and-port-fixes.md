# 2026-09-22 wlan TX bulk 停摆：归因修正 + 端口层修复

## 结论速览

1. **崩溃类缺陷已修**：usbdi shim 完成环的 memmove 压缩在突发流量下产生野指针
   （fatal core2 "wlan-wor"，FAR=0xaa0100c0）。改为无锁 SPSC 头尾下标 + 出队
   magic 校验后，长测不再 fault。
2. **端口管道层已被计数器证明干净**：修复后长测中段采样
   `tx_submit=618 tx_complete=618 / rx 3312:3311 / post_drop=0 guard_drop=0
   kill=0 wd_timeouts=0`——USB 完成零滞留、队列零丢失、lwIP 全程存活。
3. **剩余停摆在无线侧**：dongle USB 层确认收下全部 618 个 TX 帧，但首
   interval（0.09 Mbit）之后空口不再投递；ping 能发出、0 回复。范围收窄到
   R92C TX 描述符/固件域（verbatim 驱动对 8188EUS 的 bulk/QoS 帧行为），
   端口侧下一步排查点是 QoS 数据帧的软件序列号与 USB agg 描述符字段。
4. **中途教训（B2 两次尝试均放弃）**：本 port 的 TX_DISABLE = 
   `_tx_thread_smp_protect`（按核递归、跨核互斥），原 CherryUSB 临界区在
   同核 ISR 嵌套下本就安全；在其上叠自旋锁（protect+spin 与裸 DAIF+spin
   两版）都会在启动枚举期挂死。protect-only 为最终形态。

## 归因过程（证据链）

### 第一轮（插桩计数器，attrib. build 950488B）

- 阶梯通：attach → wpa PTK/GTK=CCMP → DHCP .249 → net down → ping 3/3。
- `wlan trace 2` + 60s iperf：trace 洪水（RX done 每帧一打印）淹没环形缓冲，
  iperf 自身输出丢失（海森堡：trace 拖慢时序，TX 反而推进到 tx=2425）。
- 停摆后采样：`submit=9393/15862 complete=submit-1 guard_drop=0 post_drop=0
  kill=0 peak_inflight=9`——**USB 完成层零滞留**（差值 1 = 常态在飞 RX）。
- 停摆形态：ping `start to lwip_socket` 后卡死、宿主机→板 ICMP 100% 丢——
  lwIP 无响应；radio state=RUN、beacon 持续。=> 症状高于 USB 层。

### 代码审计改判（对照 pristine NetBSD）

- 真凶形状：**compat IFQ（自研 shadow 宏，零锁手工链表）跨核并发**——
  tcpip 线程 `wlan_port_xmit_urtwn`→`IFQ_ENQUEUE` 与 usbdi worker 线程
  txeof→`urtwn_start`→`IFQ_DEQUEUE` 可同时在两核进出同一队列；
  NetBSD 中该纪律由 splnet() 保证，移植后只在 ieee80211_output 三处
  if_start_lock 使用 serializer，驱动回调这条腿漏了。
- 上一会话 tx=57 冻结 + 本会话 ping 卡 socket()，均为该竞态不同表现
  （链损坏丢帧 / tcpip 线程游走野链）。

### 修复集（落点全部在适配层）

| 项 | 文件 | 内容 |
|---|---|---|
| B1a | net80211 `port/bus/usb/cherryusb/usbdi_compat.c` | 驱动回调（txeof/rxeof）与 usb taskq 回调纳入 serializer（=NetBSD splnet 纪律）；完成环改 SPSC head/tail+DMB（深 64）；出队 magic 校验；看门狗（xfer->timeout 5s，超时 usbh_kill_urb，USBD_CANCELLED 静默回收；oerrors 语义偏差以 wd_timeouts 计数代替）；分方向计数器 |
| B1b | `compat/netbsd/net/if.h` | IF_ENQUEUE/IFQ_ENQUEUE/IF_DEQUEUE 宏内加 serializer（覆盖 verbatim 调用方，如 ic_mgtq 与 ieee80211_input 入队） |
| B1c | `port/port.h`、`port/osal/embox/port_core.c` | 新增 `wlan_port_now_ms()`（embox: ktime；CMSIS: osKernelGetTickCount） |
| B3 | vendored cherryusb `port/ehci/usb_hc_ehci.{c,h}` | `usbh_kill_urb` 重做：线程上下文禁止 W1C 清 USBSTS.IAA；改为 qh->killed 软件旗标握手（IAA handler ack，kill 负责 free+complete 恰一次）；消除 spurious 源与 IAA-lost 时"跳回调+漏 QH"路径；false 路径补 remove_in_iaad/killed 清理防二次 waitup |
| B4 | vendored cherryusb `osal/usb_osal_freertos.c` | `usb_osal_sem_create` max=1 → 0xFFFF（批量 give 丢失） |
| 命令 | 两侧 `wlan_cmd(s).c` | `wlan usbstats` 计数 dump |

### 修复后板验（951496B）

- 四线门禁绿（all/freertos/ktest/gates+cleanroom）。
- 阶梯通、ping 2-5ms。
- 600s 长测：**无 fault**（对比修复前 120s 处 core2 data abort）；2min 中途
  ping 可发出（lwIP 存活）；usbstats 如上——管道层干净。
- 剩余：TX 帧全部被 dongle 收下后空口不投递（详见结论 3），10 分钟吞吐
  验收未达成，属下一轮（描述符/固件域）。

## 现存遗留

- [OPEN] bulk TX 空口投递停止（无线侧）：排查 QoS 帧 sw seq（`wh->i_seq`
  来源 net80211 ni_txseqs）、R92C TXDESC USB agg/retry 字段、92EU txdseq2
  HWSEQ 路径在 8188EUS 的适用性。
- [记录] `irq: spurious` 在 if_init（fw load 的 control transfer 阶段）
  仍偶发一条，来源非 EHCI IAA（kill 重做后），未深究。
- [记录] `wlan trace 1` 会以每 RX 帧一行的速率刷屏，拖慢 worker 时序并
  淹没串口环形缓冲（本轮改为默认无 trace + 计数器对账）。
- [记录] `wpa: unsupported key alg 0`×2 与 `gmac1 mac initialize failed`
  为既有无害打印。

## 追加（同日 UDP/TCP 对照诊断，阻塞点定位）

| 层 | 状态 | 证据 |
|---|---|---|
| iperf3 应用 | TCP：worker 卡在 send()（首 interval 后不再打印 interval 报告）；UDP：sendto 持续 -1 但测试能结束 | TCP 600s 只见 "1-2 sec 0.09"；UDP 30s 全程 `udp send error, errno -1` + "21-22 sec 0.00" |
| 主机栈（lwIP/驱动/USB） | 干净 | usbstats：TCP 停摆后 tx 618:618 零滞留；UDP 期间 `wd_timeouts=8 kill=8`——**看门狗抓到 8 个 TX URB（BE 管道全部槽位）滞留 >5s**，即 dongle 对 bulk OUT 持续 NAK（内部队列堵死），看门狗击杀回收后 submit==complete 恢复 |
| dongle/空口（根因） | **空口投递在第 1 秒末停止** | 服务端权威日志（iperf3 -s 前台）：`0-1s 52.8KB 431Kbits/s 抖动 0.454ms 丢 1/38`，之后 19s **全零**；随后 "Connection reset by peer"（客户端侧已死）。TCP 的"首窗口后停"与 UDP 的"首秒后停"同源 |

其他观测：UDP -1 洪流为次生症状（OACTIVE 时 if_snd 无界积压 + mbuf/pbuf
耗尽类失败，帧根本没到 netif）；`wlan status` 本轮 rxerr=475（前几轮为 0，
含义待查）；tx=79/txpn=68；state=RUN、beacon 持续、关联保持。

**结论**：不是 iperf/主机栈/USB 的问题——dongle 固件的空口发送引擎在持续
发送约 1 秒后停止调度，队列积压后对 USB bulk OUT 流控（NAK），且不自动恢复。
下一层排查：① 停摆期读 dongle TX FIFO 页计数寄存器（确认页只增不减）；
② 固件周期性校准/动态调整任务在 port 的 callout→usb taskq 移植路径是否
正确喂狗（NetBSD `urtwn_calib_to` 类，时标 ~1s 吻合）；③ R92C TXDESC 的
agg/retry 字段对照 spec。

---

## 2026-09-22 晚间：证伪实验 + 88E 固件 ABI 重定性（长测目标仍未达成）

### 证伪结论（板上实测，全部落 counters/证据）

| 假说 | 实验 | 结果 |
|---|---|---|
| calib callout 未跑起来 | `wlan calib` 计数 | **证伪**：fires=142（每秒一拍正常） |
| calib×ep0（500ms kill 打断 H2C）致停摆 | `wlan calib 0` + TCP iperf 120s | **证伪**：停摆依旧；全程 ctrl_fail=0 ctrl_retry=0 |
| TX FIFO 页"只进不出" | 停摆现场 `wlan reg txq` 连读 | **证伪**：VOQ/VIQ/BEQ/BKQ/MGQ/HGQ 与健康态逐位相同（0x00ff00ff 系），FIFO 空 |
| 固件 MAC 暂停 | 停摆现场读 0x522 | **证伪**：TXPAUSE=0x00 |
| H2C 信箱卡死（初态） | 停摆现场读 0x1cc | 初期 0，**后期非零**：信箱最终不排空 |

### 停摆实测形态（多轮一致）

- 触发：持续空口 TX 约 1.2~2.4MB（~1-3s 满速）后，**dongle 停止空口调度**；
  USB bulk OUT 层面继续"正常完成"（usbstats 严格配对、无 kill）→ 数据积压在 dongle RAM。
- ep0 vendor 读从毫秒级劣化到 ~13s/次（8051 半瘫），RX（beacon）全程正常。
- MAC/FIFO/DMA 寄存器全静默；TXDMA_STATUS 0x401→0x3621 的变化是 watchdog kill 的果不是因。
- 外证：**NetBSD PR #59036**（真 NetBSD+真 8188EU 同款停摆：device timeout 风暴 + "could not
  send firmware command 5" + 永不恢复）；FreeBSD 论坛同款。上游驱动+固件配对本身就是坏的。

### 关键发现：88E 固件 H2C ABI 与 92C 完全不同（rtl8189fs 树内 rtl8188e_cmd.h 实证）

厂商 88E 命令表：`MEDIA_STATUS_RPT=0x01、PS_PWR_MODE=0x20、MACID_CFG=0x40(7B)、RSSI_REPORT=0x42(4B)`。
NetBSD 驱动对 88E 发的 `MACID_CONFIG=6 / RSSI_SETTING=5`（92C 编号）在 88E 固件里**全是死编号**——
固件 RA 表从未被编程过；calib 每秒喂的 cmd5 是无效命令。

### 已落地的适配层工作（两仓镜像同步，verbatim 零改动）

1. `wlan reg read/write/txq`（hex 解析本地实现，绕过 cherrysh strtoul 忽略进制的缺陷）+ 19 寄存器停摆取证集。
2. `wlan calib 0|1` callout 抑制开关 + fires/sched/suppressed 计数（双 osal 同形）。
3. usb_add_task 满环丢弃从静默改计数+首打印；usbstats 扩展 ctrl_fail/ctrl_retry/task_drop/task_busy。
4. **88E 固件维护钩子**（urtwn_reg.c，适配层）：88E ABI 直写信箱（HMEBOX+4B EXT），
   关联时发 MEDIA_STATUS(0x01,{1}) + MACID_CFG(0x40){macid4=BC basicrates, macid0=BSS rates} +
   INIDATA_RATE_SEL；RUN 后每 2s 喂 RSSI_REPORT(0x42, avg_pwdb)。`wlan ra 0|1` / `wlan fwfix` 控制。
   失败不记录、下拍重试。/ `wlan fwfix`。|
   （教训：初版载荷 JOINBSS_RPT byte0=0=mstatus"断开" + SET_PWRMODE 全零 payload 是瞎猜，
   曾致固件更快退化——载荷必须逐位按厂商头文件。）

### 效果与新形态

- 钩子（正确 ABI 版）：关联→DHCP→iperf 首秒 **1.19MB @ 9.87 Mbps（历史最高）**，60s 时
  tx 920:920 全配对、wd_timeouts=0；但 dongle 在 ~1.2MB 后仍停止空口调度（服务端 idle timeout）。
  stop 之后 H2C 信箱不排空（cmd5 超时）→ H2C 级复活不可行。
- 10 分钟长测目标**未达成**。

### 下一轮线索（按优先级）

1. **换固件镜像**：现内嵌 rtl8188eufw.bin == NetBSD 同款（sha 1241ddbf…）；Linux 生态跑同一
   dongle 的是 linux-firmware 的 `rtlwifi/rtl8188efw.bin`（v29 系）——很可能是不同构建。
   vendor 驱动+vendor 固件是验证过的组合。对拍两个 blob；若不同，换 Linux 版固件重测。
2. **USB_TX_AGG**：厂商 88E USB 默认开 TX 聚合，NetBSD 显式清 AGG_EN（if_urtwn.c:4925-4927）。
   厂商固件按聚合场景调过；在钩子里开 AGG_EN + 聚合参数重测。
3. KEEP_ALIVE(0x03)/PWR_MODE(0x20) 按 SETPWRMODE_PARM 正确载荷补发。
4. RSSI 喂食确认 avg_pwdb 在 TX-heavy 下是否真的更新（可能恒 -1 → 从未喂过）。
5. 观察项：`irq: spurious` 偶发出现，来源未查。
