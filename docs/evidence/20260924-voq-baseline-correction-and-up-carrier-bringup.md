# 2026-09-24 · VOQ 基线修正 + UP 载体无线阶梯重放（D57 场景 7）

分支 `wip/wireless-gdb-integration`（fwc-integration worktree），本轮提交
`aa88f12`（reaper 死锁修复）+ `5433140`（场景 7 wl-load + 门禁 autorun）。

## 1. 现役 SMP 构建（fwc-urtwn ea270c5 / sha 7e92c88e）上的 wedge 基线校准

两趟满载窗口均未复现 wedge（kill=0、wd_timeouts=0、usbstats 账目全平、submit_fail=0）：

| 窗口 | 条件 | 结果 |
|---|---|---|
| 30s TCP 上行 | `wlan trace 2` 开 | 4.50 Mbits/sec，全程干净 |
| 10s TCP 上行 | 无 trace | **11.51 Mbits/sec**（≈N6 包络 12.2M），干净 |

**重要基线修正——上一轮的"VOQ jam"取证解读被推翻**：健康系统在 30s+10s 满载之后，
`VOQ_INFO(0x400)=0x02ff00ff`、`TXDMA_STATUS(0x210)=0x401`、`TXPAUSE(0x522)=0x00`、
其余队列 `0x00ff00ff`——与上一轮 wedge 态寄存器转储**逐位相同**。即：
`0x02ff00ff` 是 TX 后健康稳态，不是"积压 512 包"的 jam 签名；wedge 的设备侧签名
在这 19 个寄存器快照里不存在。wedge 的可靠事实只剩宿主侧：bulk OUT 完成停止 +
不自愈 + RX/ep0 存活。后续取证应改用**指针运动双读法**（同一寄存器隔 ~1s 读两次
对比排放进度），静态值不构成证据。

其它：`wlan trace 2` 打印本身拖垮吞吐 4 倍（11.5M→4.5M），取证时严禁开 trace 打印；
ctrl 打印有"开机 800 条"预算（`wlan_ctrl_trace_seq`），关联阶段即耗尽，SMP 构建上
无法用它测 ep0 频率（复位仅发生在 if_init 前）。**ep0 频率精测移交 UP 载体**
（gdb 直接读 `wlan_ctrl_trace_seq` t0/t1）。

## 2. UP 载体（threadx-uc）无线阶梯重放：首次把 USB/无线栈跑起来

### 2.1 载体工程学改造（5433140）

- **门禁 autorun**：`dbg_scenario` 门改为 "==0 才 park"，每次运行后清零；
  `dbg_scenario` 默认 7 → 冷启动自动跑 wl-load，console 全程可见，gdb 只在
  park 时接入（也修掉"跑完重跑"的非幂等风险）。
- **场景 7 wl-load**（`port/adapters/net80211/wl_load_scenario.c`，UP-only 编入）：
  usb_start 独立线程（**multi-HCD 构建里 usb_start 是常驻热插拔看门狗，永不返回**
  ——主线跑独立任务，串行调用会堵死阶梯）；connect 每 10s 重发 + 轮询 DHCP 地址；
  满载用 `iperf3_client_start`；停摆监视器读 lwIP `tcp.xmit/recv`，冻结
  `dbg_wl_stall_secs` 后**先**抓 VOQ/TXDMA 到 `dbg_wl_*` 静态变量（ep0 此时还活着，
  park 之后 CPU 停了就没有人驱动 ep0）**再** `gdb_break()`。
- 参数（SSID/pass/host/时长/阈值）全为全局变量，gdb `set var` 可覆盖。

### 2.2 gdb 板上工作流（本轮实跑沉淀）

- 会话内 attach：gdb 走 SerialHub raw TCP 18000（gdbinit.uc）；会话期 console
  静默门会**丢弃** board_log——要看日志就必须 detach（autorun 形态下正是如此）。
- **运行中 break-in**：目标自由跑时 stub 不应答 RSP；先经 RW 桥发原始 `0x03`
  （python socket），会话外的 tick 轮询捕获后 `gdb_break()` park，再 attach。
- **被中断线程 PC 取法**：tick-park 的 trapframe 是 ISR 自己的帧（ELR=gdb_break）；
  被中断线程的现场在 `_tx_thread_context_save` 存进 TCB 的 `tx_thread_stack_ptr`
  指向的帧里：`[0]=SPSR`、`[1]=ELR`。本轮靠它定位自转点。
- ThreadX 线程链走查脚本（`/tmp/walk.gdb` 模式，py3 版 gdb：
  `aarch64-none-elf-gdb-py3`，普通版无 Python）：`_tx_thread_created_ptr` 环链
  （**注意环形终止**）+ 读 TCB name/state/run/stack_ptr。
- 批处理 gdb 死亡（timeout 杀）会向 stub 发 `$vKill/$k` 把 RSP 会话搞乱——脚本
  必须 finally detach；死了就冷启动重来。

### 2.3 找到并修复一个真 bug：CherryUSB ThreadX osal reaper 死锁（aa88f12）

首轮场景 7 在 usb_start 冻结（stuck 6 分钟）。gdb 走查链：
usb_osal reaper（prio 10）READY+CURRENT、SP 恒定、被中断 ELR 落在
`_txe_queue_receive` 的 `TX_PTR_ERROR` 返回路径 → `usb_osal_mq == NULL`。

根因：`usb_osal_init` **先**创建 reaper（prio 10，创建即抢占）**后**创建/赋值
reaper 的 mq → 单核上 reaper 首个 `mq_recv(NULL)` 返 `TX_PTR_ERROR` → `continue`
原地自转 → 创建者（prio 19）被永久饿死，mq 永远建不出来。SMP 上同样的竞态被
"创建者在另一核上继续跑几微秒"掩盖——**所有 ThreadX lane（含 SMP 主线）都带着
这个雷**，单核载体第一次把它引爆。

修复：先 `usb_osal_mq_create(32)` 再 `usb_osal_thread_create("usb_osal",...)`。
板验：reaper 正常阻塞（state=TX_QUEUE_SUSP），枚举/attach 全流程通过。
**待同步**：`wip/rtw8189f-sdio` 等其它 ThreadX lane 共用此文件。

### 2.4 UP 载体阶梯现状（rtw88u 设备在场）

- 枚举 + rtw88u（RTL8821CU, 0bda:c820）attach + 固件加载：**PASS**
  （`RTL8821CU ready (firmware 24.11.0)`，MAC 54:c9:e0:00:29:66）。
- **前沿：wpa 认证响应收不到**——扫描/信标 RX 正常（能看到全部 BSSID），
  但 AUTH 响应超时，supplicant 在多 BSSID（mesh）间轮试全部失败。
  SMP 主线同栈全阶梯 PASS，属 UP 载体（单核 -Og 时序 / xHCI RX 路径）新前沿。
  **注意：本轮板上的 dongle 是 8821CU；urtwn(88E) wedge 实验需要换插 8188EUS。**

### 2.5 排查中排除的假设（勿重查）

- EHCI `USBINTR=0`：是 U-Boot preboot `usb start` 的遗留形态，随后发现枚举走的是
  xHCI bus2，与此无关。
- `_tx_thread_preempt_disable` 泄漏：实测 0。
- `_tx_thread_system_state=1`：是 tick-park 探针自身处于 ISR 嵌套内的合法读数，
  不构成"调度器没清状态"的证据。
- UP port 挂起机制（system_suspend/system_return/schedule asm）：与 SMP diff 均
  为预期单核差异，reaper 修复后阻塞路径工作正常，port 本身无辜。

## 下一步

1. **88E wedge 主线**（需插回 8188EUS）：UP 载体跑 urtwn 阶梯 → 满载等 wedge →
   自动 park → 冻结取证（宿主 EHCI QH/qTD + shim toggle/xq + 线程走查；
   设备侧 VOQ 用指针运动双读法）；ep0 频率用 gdb 读 `wlan_ctrl_trace_seq` t0/t1。
2. rtw88-on-UP 认证 RX 前沿：优先在 SMP 构建复测同 dongle（若 SMP 秒连，则锁定
   UP 载体特有时序因素）。
3. aa88f12 同步到 wip/rtw8189f-sdio（及后续合入各 ThreadX lane）。
4. memory 已更新：单步可用定案、VOQ 基线修正、reaper 死锁。
