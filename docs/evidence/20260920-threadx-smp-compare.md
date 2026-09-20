# D39 内核对比线：ThreadX SMP 4 核镜像上板验收（第一轮对比）

- 日期：2026-09-20
- 镜像：`build/rk3568-threadx/threadx-smp.bin`
  - 变体 v1（tick 风暴版）`f7a9c6e8…` ×1 轮
  - 变体 v2/v3（wrapper 重臂 / affinity）`d69d65d4…` / `85fcfe3b…` ×2 轮（教训轮，见下）
  - **终版 `151eee4c…` ×2 轮**（board 验收口径的同镜像 ×N）
- 对照基线：FreeRTOS 4 核主镜像（20260919 issue：shell 100% 无响应，stash 对照实证）
- 状态：**对比线主结论成立**；ITS LPI 一项 open（ThreadX 镜像侧）

## 判别结论（本对比线要回答的问题）

**"4 核 shell 无响应"问题在 FreeRTOS SMP 侧，不在板层。**

| 工况 | FreeRTOS 4 核（issue 记录） | ThreadX SMP 4 核（本轮） |
|---|---|---|
| 启动锚点 | 全绿 | 全绿（ITS 除外，见 open） |
| 4/4 核上线（PSCI） | ✔ | ✔ `smp: 4/4 cores up, psci 1.1` ×5 轮 |
| shell 字符回显/交互 | ✗ 零回显零响应（100% 复现） | **✔ 5/5 轮全程响应** |
| `uptime` 执行 | ✗ | ✔（修复后 1ms/tick 精确） |
| `rtos` 原语自检 | ✔（1 核形态） | ✔ `semaphore/queue/mutex OK` |
| console RX→shell 唤醒链 | ✗ 死 | ✔ INTID 150 → ISR → `osThreadFlagsSetFromISR` → 唤醒 |

同一 app（main.c 全部锚点线程）、同一板层（gicv3/tick/console/mmu/PSCI，仅两符号中性化
`817335e`）、同一 cherrysh——唯一差异是内核。板层的 console RX IRQ→ring→FromISR→
shell 唤醒链、GIC-600 四核初始化、PSCI 二次引导、tick 单 tick 模型全部被 ThreadX
证明工作正常。FreeRTOS 侧的排查应聚焦其 SMP port/内核（从 20260919 issue 的
"静默冻结"形态继续）。

## 5 轮冷启动记录（oslab，全部 KI-001 冷断电 ≥6s + settle 12s）

| 轮 | 镜像 | 结果 |
|---|---|---|
| 1 | v1 `f7a9c6e8` | 4/4 核、锚点齐、shell 响应；发现 tick 风暴（uptime 145563298 ticks/~40s ≈ ×1800）；its FAIL |
| 2 | v2 `d69d65d4` | 同形态；tick 风暴依旧（修复加错了位置，见教训）；its FAIL |
| 3 | v3 `85fcfe3b` | + affinity 映射；its FAIL |
| 4 | **v4 `151eee4c`** | **tick 精确（69.613 s/69613 ticks）、rtos OK、smp 4/4 reported、shell 响应** |
| 5 | **v4 `151eee4c`** | **uptime 60.014 s/60014 ticks、shell 响应** |

## 过程中修掉的确定性 bug（提交 `54a9b4f`）

1. **tick 重臂不在分发路径上**（v1/v2 教训）：`tx_irq_handler` 的 tick 分支直调
   `_tx_timer_interrupt()`，`OS_Tick_Setup` 注册的 `tx_tick_wrapper` 根本不参与分发
   （FreeRTOS 镜像同构——注册的 handler 也不是真正执行者）。level 触发的 CNTPNS
   线在 EOI 落下瞬间重新 pending → core 0 tick 风暴（INTID 30 路由 core 0），
   其余 3 核正常所以 shell 还活着——这正是"uptime 冲到 40 小时但 shell 能用"的
   成因。修复 = 重臂（`OS_Tick_AcknowledgeIRQ`，TVAL 重装即线清除）进分发路径。
2. **`osThreadNew` 丢弃 `attr->affinity_mask`**：smp_test 的每核绑核经 CMSIS attr
   传入，ThreadX 版忽略后任务全核漂浮。已映射到 `tx_thread_smp_core_exclude`
   （补位图，TX_THREAD_SMP_CORE_MASK）。

## 遗留 / open

1. **ThreadX 镜像 ITS LPI 投递不达**（4 次运行确定性复现，与 tick 风暴无关——
   风暴修复后仍 FAIL）。itsdump 存证：GITS enabled、CWRITER==CREADR（队列消化完）、
   DTE/ITE valid（e000:0 → pINTID 8192）、prop 全部 b1（enabled, prio 0xb0）、
   pend 0、collection → rd_target 0。FreeRTOS 4 核锚点 its PASS 是既有记录
   （20260918 round10），差异归因未做。方向：ITE 的 ICID 字段语义与 collection 表
   容量核对、LPI 门铃在 ThreadX 运行态的投递条件。
2. **smp 命令 `TIMEOUT, 4/4 tasks reported`**：smp_test.c 无 done-flags setter
   （master 既有形态，FreeRTOS 镜像同输出）。有信息量的部分是 4/4 任务 ×20000
   samples 全部完成（含每 256 次 osDelay(1) 的跨核唤醒）——跨核 delay 唤醒链在
   ThreadX 4 核下正常。修 setter 属 app 侧独立小改动，未夹带。
3. `ITS LPI FAIL` 使锚点不完全等价于 FreeRTOS 4 核"锚点全绿"；对比结论基于
   shell/console/tick/smp 负载路径，不受此项影响。

## 审计口径

- 今日实测：5 轮冷启动全部现象、uptime/rtos/smp/itsdump 串口输出、5 轮镜像 sha256
  （deploy 前后对拍）。
- 构建通过：`make all`（sha256 与改动前逐字节一致 fbee0d6c…）、`k4`、`gates`、
  `ktest`；`make threadx`。
- 未验证：FreeRTOS 4 核 + GMAC/USB 负载下的 ThreadX 等价工况（ThreadX 镜像
  net/usb 为 stub）；its FAIL 归因。
