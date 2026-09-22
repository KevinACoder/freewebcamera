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
