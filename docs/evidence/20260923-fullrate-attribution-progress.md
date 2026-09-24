# urtwn 满载失稳归因进展：IRQ 补锁被证伪 + 三条单核路径全败 + 新证据链

日期：2026-09-23（晚）　分支：wip/urtwn-usb　前序：4432a4f（两形态定性）

## 本轮做了什么

1. 源码审查：`usb_osal_enter_critical_section`（ThreadX port）= `TX_DISABLE` =
   `_tx_thread_smp_protect`（反汇编证实），线程侧本就有跨核保护；**真正裸奔的是
   IRQ 侧完成路径**（scan/check_qh/waitup 从不进临界区）。
2. 修复候选：在 `usbh_glue.c` 给 EHCI 中断分发包上同一把锁（166dead）。
3. 板验该候选：**满载 fatal 原样复现（FAR=0x580008c0 与上轮完全同值）→ 候选
   不是本 fatal 的修复**。补锁保留（它关闭的窗口真实存在，审计安全）。
4. 两层插桩（sem_give 句柄校验 + waitup 同步分支全量打印）拿到决定性证据。
5. 评估 ThreadX 单核判别实验的三条载体 → 全部不可用（见下）。

## 判别实验载体评估（用户问"freertos 能跑吗"）

| 载体 | 结果 |
|---|---|
| 编译期 `SMP_CORES=1` | 内核 init 第一步即死（`tick armed` 后无输出）|
| 运行期 `_tx_thread_smp_max_cores=1`（内核 DYNAMIC_CORE_MAX 机制）| init 全过，但**调度器永不分派新线程**：prio 0 hub 线程连第一行都没跑到，main 卡死在 usb_start |
| 逐线程 `tx_thread_smp_core_exclude` 钉 core0（CMSIS/osal/timer 三处创建点）| shell READY 都出来，但 shell 对键入无响应、hub 线程依旧没跑 |

结论：**ThreadX 单核形态在 ports_smp 上没有捷径**。FreeRTOS 单核能跑（urtwn 全链
历史闭环 2.34M），但其排序本就正确（worker=FreeRTOS 2 < tcpip 4）、速率上不了
~14M，判别力为零。真正出路 = 导入上游非 SMP port（`ports/cortex_a55` + 非 SMP
common，vendored 树只有 SMP，属新 vendor 引入，中等工作量）。实验性钩子已全部
撤销，知识记录在案。

## 满载失稳新证据链（诊断构建 6d7e2b13 / cc0c0aac）

**证据 1（插桩命中）**：`usb-osal: BAD sem give 0xd51be000580008c0 RA 0xa036e70`
- 调用方 addr2line = `ehci_urb_waitup:753` 同步分支（`urb->timeout != 0` →
  `sem_give(qh->waitsem)`），从 **IAA 中断的池遍历**（:1610）进入，IRQ 上下文。
- 坏句柄跨构建**同值**（低 32 位=0x580008c0，即 fatal 的 FAR）→ 确定性损坏，
  非随机竞态；且 IRQ 补锁生效下仍发生 → 坏状态在遍历前已存在。

**证据 2（SYNCGIVE 洪泛）**：ep0 控制通道（同一 `ep0_urb` 0xa286bc8，两个 QH 槽
0xa2c4880/0xa2c4980 交替、wsem 各异）以 **~100Hz** 持续走同步完成路径，从 assoc
一直打到崩溃点，字段全部"健康"（hcpriv==qh、inuse=1）。控制 urbs 的完成/回收
握手异常：sync 分支完成不摘链不释放（双槽轮转、槽位长期 inuse）。

**证据 3（本次 fatal 全链）**：
```
[wlan] usbdi watchdog: killing xfer stuck 5045 ms (timeout=5000)
fatal[0] core1 ESR=96000004 FAR=b27b00d0
  thr name=wlan-wor → usbd_shim_urb_work usbdi_compat.c:769（SLIST_REMOVE 时
  xfer->pipe 为垃圾指针）
```

## 综合定性（当前最佳假设）

- **一阶故障 = bulk 完成流停摆**（xfer 5s 无完成，即前轮"静默横死"B 的本体）。
- **fatal 是二阶引爆**：watchdog 杀挂起 xfer → kill/unwind 路径遇损坏指针。
- 同步控制路径（ep0_urb + IAA walk + sync 分支无回收）是已证实的行为异常区，
  与 bulk 停摆的因果待定：要么共享同一上游根因（async 环/QH 生命周期），要么
  控制通道本身把环状态搅坏后拖死 bulk。

## 下一步（按优先级）

1. **审计 ep0_urb/control 的完整生命周期**（usbh_control_transfer、core 的
   submit/wait、kill 路径）：sync 完成后 QH 由谁回收、IAA walk 与 kill 握手
   （killed=1→2）与重提交的交错。重点：单 urb 复用 + timeout!=0 路径。
2. 给 bulk 停摆的**起点**插桩（usbstats hcd 桶 + arm/done 时间戳已有），抓
   "最后一个完成"与"第一次超时"之间的环状态（QH 链、qTD token）。
3. watchdog kill 路径本身加防御（pipe/状态校验）暂缓——先定位一阶根因，避免
   又一层"无害就加"。
4. ThreadX 单核：待非 SMP port 导入后再立判别实验（纪律：单核跑通再上 SMP）。

## 板卡与提交

- 板卡：借用 8821cu 会话空闲窗口（串口静默 24min+、断电状态），完毕
  `power_off {force:true}` 复核 power_on=false。
- 提交：166dead（IRQ 补锁，含"未修复本 fatal"的诚实说明）；诊断插桩未提交，
  已按"不需要就不加"全部还原；构建回 `27cc72af`（与板测镜像一致）。
