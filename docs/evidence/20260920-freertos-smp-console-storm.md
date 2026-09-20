# FreeRTOS SMP 控制台风暴定位：INTID 150 卡 ACTIVE、tick 线饿死（本轮 open）

- 记录日期：2026-09-20
- 提交：`bf2d75b`（统一带戳日志）→ `e6dac4b`（虚假分类 + 可恢复启发式）→ `478c36d`（风暴状态机 + 转储）
- 前序：`20260919-issue-smp-shell-console-rx.md`（症状 A/B）、`20260920-threadx-smp-compare.md`（D39：FreeRTOS 4 核 shell 死 / ThreadX 5/5 活，板层无辜）
- 状态：**根因链已闭环至"INTID 150 卡 ACTIVE、EOI 不 deactivate、tick 线被饿死"；最后一步归因（EOImode / 线拉高首因）open**

## 1. 新工具：统一带戳日志（本轮第一产出，立即兑现）

全树启动/调试打印统一为 NetBSD dmesg 形状 `[   s.mmm] module: message`（CNTVCT 基
boot 相对时钟，锁汇点加戳 + ISR/下降沿用无锁 raw 加戳变体）。锚点改模块化措辞
（`shell: READY`、`app: M0 ANCHORS DONE`、`fatal: ...`、`uart: ...`、`smp: ...`）。
shell `uptime` 双时钟对拍（cntvct + tick）；`version` 按镜像标识报告内核名。
新 `gicdiag` 命令（PMR/RPR 快照）。**本轮全部板验结论直接来自带戳时间线。**

## 2. 关键板验事实（全部今日实测，冷启动纪律）

### 2.1 旧的"3 击永久禁用"就是 shell 死因（症状 B 闭环，症状 A 同根）

旧驱动把 `iir=07`（16550 bit0=1 = **无中断挂起**，不是线状态 0x06）当坏命中计数且
双重自增，有效阈值=3，3 击后 `IRQ_Disable(150)` 永久关闭 → shell 死。带戳日志显示
这些是**连续风暴的前三个事件**，旧代码在 50ms 内自杀式禁用，把风暴藏成了"安静的死"。

### 2.2 风暴的精确定性（单核 =4 核一致）

FreeRTOS 单核镜像（CNTV/27）干净时间线：

```
[   0.012] tick: INTID=27 cntv armed, load=24000
[   0.016] uart: arm: intid=150 pend=1 ier=00 lcr=03 icfgr=00000000
[   0.022] uart: rx irq entry: intid=150 iir=07 lsr=00
```

- **`pend=1` 早于使能、IER=00**：INTID 150 在控制台从未使能时已被置 pending；UART 自身
  无任何条件（IER 掩蔽一切），lcr=03 排除 DLAB 错位。
- **`icfgr=00000000`**：level 触发配置正常。
- 风暴规模：≈19.5 kHz 持续（4 核镜像 200 s 内 n>400 万，`iir=07 lsr=20/00`）。
- **`ICPENDR` 清除无效**（`latch_after=1` 恒成立）→ 不是陈旧软锁存，是线级来源。
- **`gicd_act150=00400000`**：INTID 150 的 **ACTIVE 位恒卡死**（GICD_ISACTIVER word4
  bit22）——EOIR1 写入从未将它 deactivate。portasm_smp.S 的 IAR 保存/恢复经核对无误
  （STP/LDP X0），EOI 值正确 → **EOImode 语义是头号嫌疑**（无法直接读，见 2.4）。
- **tick 线被饿死**：单核 `gicr_pend=08000000`（PPI27 pending）、4 核对应 PPI30——
  150（优先级 0xb0）比 tick（0xd0）紧急，GIC 永远先报 150，tick 永不运行 →
  osDelay 全死 → **"锚点全绿后静默冻结/ shell 无响应"（症状 A）的机理实锤**。
- 形态二（同日 =4 某轮）：无任何 spurious 打印，0.446 s 起纯 `!! tasklock/isrlock
  acquire timeout` 周期刷屏——同一根源的另一表现（风暴打印被饿死场景/时序差异）。

### 2.3 ThreadX 对照（同板层同驱动同 shell）

ThreadX 4 核全新带戳日志一轮冷启动：锚点全绿（`smp: 4/4` 0.059、tick 0.063、
`shell: READY` 0.067、`app: SPI SOFTTRIG OK` 0.675），**零幻影、零风暴、零 uart 中断**
（rx trace 未打印 = handler 从未进入）。风暴是 **FreeRTOS 镜像特有、与 SMP 无关**
（单核也有）、与内核 IRQ 路径或镜像启动形状相关。

### 2.4 新坑登记：ICC_CTLR_EL1 读取 trap

`mrs s3_0_c12_c11_4`（ICC_CTLR_EL1，读 EOImode 的正路）在本分区**同步异常**：
`ESR_EL1=02000000`（IL=1，EC=0 Unknown）、FAR=0——与 D33 的"ICC_IAR0 读 → EL3 静默
复位"同族（firmware 拦截）。EOImode 只能靠写 ICC_DIR 试效或换 NS 视角验证。

## 3. 修复与防御（已提交，四形态构建 + gates 全绿）

1. `e6dac4b`：虚假（IIR bit0=1）与真坏命中分类；单次计数（修双重自增）；坏命中到限
   从"永久 IRQ_Disable"改为 **armed-off（IER=0）+ 可重臂恢复**；驱动导出
   `uart_console_rx_down/kick`；shell 任务 500 ms 预检自动重臂；重臂恢复调用方的
   Receive 缓冲（不再回到驱动内部缓冲，修正 kick/rebind 的缓冲所有权隐患）。
2. `478c36d`：每次全量武装前打 pre-arm 快照（钉住线何时拉高）；虚假路径先清一次
   ICPENDR（陈旧锁存世界即自愈）；line-world 回退：1000 次线级命中后 IRQ_Disable
   保住核；每 128 次转储 GICR_ISPENDR0/ISACTIVER0 + GICD_ISACTIVER(150)。
3. 未修（本轮证据足够、修复需下一轮定位 EOImode/首因）：150 卡 ACTIVE 本身。

## 4. 下一步（按优先级）

1. **EOImode/ICC_DIR 归因**：风暴中写 ICC_DIR（S3_0_C12_C11_1）deactivate 150，看
   active 位与风暴是否立即终止（一次冷启动可判）；或检查固件是否以 EOImode=1 交付
   （BL31/OP-TEE 的 GIC 配置对 NS EL1 的可见影响）。若 ICC_DIR 也被 trap，改从
   "150 交付路径"查（首因为何是 150 而非 27/30——GIC-600 时序/错误响应？）。
2. **线拉高首因**：pend=1 在 0.016/0.067 s，紧随 tick arm；对比"无 tick"变体与
   U-Boot 侧 ISPENDR/ISACTIVER 快照，剥离"固件遗留"vs"首次 tick 交付错位"。
3. 修复落地后重跑本文件 §2 的对比矩阵（FreeRTOS =4 ≥5 冷启动 shell 交互 + soak），
   才能宣布 20260919 issue 症状 A 闭环；症状 B 已闭环（§2.1 + 修复 1）。

## 5. 今日冷启动台账（摘要）

| 轮次 | 镜像 | sha256(前8) | 形态 | 结论 |
|---|---|---|---|---|
| boot-231422 | FreeRTOS =4（bf2d75b） | 0e15b9be | 风暴 n>8.8万@4.5s，`!! tasklock` | 旧阈值3击假象被戳破：是持续风暴 |
| boot-232130 | ThreadX =4（同提交） | d61e2d7d | 锚点全绿，零幻影 | 对照干净（日志改造无回归） |
| boot-232416 | FreeRTOS =4（e6dac4b） | 15262249 | latch=1，ICPENDR 清不掉，n>400万@200s | 线级来源实锤 |
| boot-233603 | FreeRTOS =4（478c36d） | 77b6b243 | `arm: pend=1`@0.067，形态二（纯 tasklock） | 风暴另有表现形态 |
| boot-234120 | FreeRTOS =1（478c36d） | b031b74b | `arm: pend=1`@0.016 + 完整 storm 转储（act150 卡死、PPI27 pending） | 单核同病；机理闭环 |
| boot-234611 | FreeRTOS =1（ICC_CTLR 试读） | d178f580 | mrs ICC_CTLR → 同步异常 fatal | 新坑登记（2.4） |

## 6. 口径

- **今日实测**：§2 全部带戳时间线与状态转储；ThreadX 对照。
- **构建通过**：四形态 + cleanroom-scan/check-deps PASS（478c36d）。
- **未验证/未归因**：EOImode 写 ICC_DIR 试效、线拉高首因、修复后的 =4 shell 验收。
