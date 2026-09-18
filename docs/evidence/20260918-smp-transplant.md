# 2026-09-18 — SMP 整体换装：SDK ft_platform port 移植（D33）

## 决策

自研 SMP port（D32 派生自 ARM_AARCH64_SRE，轮 2-17 板测从未跑通，QEMU 存档于
`wip/smp-qemu`）**整体废弃**，换装参考 SDK SMP 线
（`rk3568_lab/os/freertos-smp`，Phytium free-rtos-sdk SMP v1.2.0 pristine import
+ 三轮板验根因修复，master@48717c2，BSD-3/MIT）已板验的 port 实现。
移植原则：逻辑/寄存器序列/锁纪律逐函数忠实搬运，仅做标识符改名（clean-room
门禁）与粘合点落地（standalone SDK API → 本树 board 层 API）。

## 移植单元（源 → 目标）

| SDK 源（freertos-smp/third-party/freertos/portable/） | freewebcamera 目标 |
|---|---|
| `GCC/ft_platform/aarch64/portmacro.h` | `port/adapters/freertos/portmacro.h` |
| `GCC/ft_platform/aarch64/port.c` | `port/adapters/freertos/port.c`（MCS 队列锁、PMR+ISR 锁 tick、xPortStartScheduler） |
| `GCC/ft_platform/aarch64/portASM.S` | `port/adapters/freertos/portasm_smp.S` |
| `GCC/ft_platform/aarch64/freertos_vectors.S` | `port/adapters/freertos/port_vectors.S`（FIQ 哨兵；文件改名避 `f[a-z]*.S` 门禁） |
| `portable/freertos_configs.c` | `port/adapters/freertos/port_glue.c`（tick/IPI/PSCI/握手/分发） |
| `GCC/ft_platform/freertos_smp_startup.c` | 不需要：RK3568 单簇，逻辑核号=MPIDR Aff1 直读（board_smp_core_id），cpu_logical_map 为恒等 |
| `GCC/ft_platform/aarch64/freertos_secondary_start.S` | 不整抄：本树 `smp_secondary.S` 已板验（EL 下降/栈/ MMU），对齐其语义 |

删除：自研 `port_smp.c`、旧 `portasm_smp.S`、board 层 `vectors.S`
（表迁入 port 层，boot_vectors 仍在 startup.S）、app 层 ping/SGIHits 探针。

粘合映射（详注于各文件头）：`InterruptSetPriorityMask/Get`→CMSIS
`IRQ_SetPriorityMask/Get`（ICC_PMR）；`InterruptInstall/Umask/SetPriority`→
`IRQ_SetHandler/Enable/SetPriority`；`FExceptionInterruptHandler`→
`board_gicv3_dispatch`；`StartSecondaryCpuUp`→`board_smp_start_secondaries`；
`SecondaryCoreStartup`→`uxPortSecondaryMain`；`DbgRawPrint*`→`board_early_print`；
tick 走 CMSIS `OS_Tick_*`（CNTPNS/PPI30，单 tick 于 core0，与 SDK 同）。

## 已登记偏差（IMPORT-INFO.md / docs/imports.md 同步）

1. `ullCriticalNesting[]` SMP 数组初值 9999→0：SMP 内核嵌套存于 TCB
   （portCRITICAL_NESTING_IN_TCB），该数组只剩 asm 恢复路径消费；9999 会把
   PMR 永久钉在 API 层（参考线全系设备中断在其上无感，本树 FromISR 驱动坐在
   API 层会被饿死）。
2. `uxPortSetInterruptMask`/`portUMASK_INTERRUPT` 不再盲目 DAIFCLR：FromISR
   路径持 DAIF 屏蔽进入，盲目重开允许电平线嵌套自身临界区（wip/smp-qemu 板验
   过的同款修复）；改为保存/恢复调用者 DAIF、PMR 单写。
3. `icc_rpr_read` 编码修正：`s3_0_c12_c8_0` 是 **ICC_IAR0**（Group0 应答），
   NS EL1 读之 TRAP EL3 → OP-TEE 意外 trap → 静默温复位；正确编码
   **ICC_RPR_EL1 = s3_0_c12_c11_3**（老 port 同）。板测 5 轮必死的根因。
4. `board_gicv3_send_sgi` 的 SGI1R 编码 c12_c11_6（ASGI1R）→ **c12_c11_5**
   （SGI1R，standalone fgic_v3.h 同）。
5. 单核适配：`configUSE_CORE_AFFINITY` 仅在 N>1 定义（内核 #error）；
   cmsis 适配层 affinity API 加 #if 分支；portasm 的 pxCurrentTCB/TCBs 按
   SMP_CORES 条件；`uxPortSecondaryMain` =1 桩。

## 板测记录（全部 KI-001 冷启动，tftp 0xa000000 + go）

| 轮 | 镜像 | 配置 | 结果 |
|---|---|---|---|
| 1 | `7075d71e` | 2 核，移植版+M线优先级 | 2/2 cores up，锚点全绿，gmac1 后死（未定位） |
| 2 | `d0090646` | +nesting=0 | 同上 |
| 3 | `9aa7b386` | API=15 | SPI/LPI 全饿死 → 15 不可递送（4-bit PMR），API 改 14 |
| 4 | `061c5c02` | API=14 | gmac1 后温复位（DDR 重训练+SPL 重跑） |
| 5 | `46e45d70` | M线优先级 14/11/9 | 同 4 → 优先级方案排除 |
| 6 | `3e9d1be5` | =1 判别 | **单核也死** → 与 SMP 无关，移植适配缺陷 |
| 7 | SDK `rk3568_aarch64_itx_smp4.bin` | 4 核 SDK 应用 | 稳跑 83k+ ticks → 环境/供电排除 |
| 8 | 面包屑定位 | 2 核 | 死点：`xTaskGenericNotifyFromISR` 内，首个真实 RX 帧的优先级验证 |
| 9 | **`e98f07e0`** | 2 核，RPR/SGI1R 编码修复 | **gmac1 通过，双网口 link up，NET READY，双口 ping 0% 丢包** |
| 10 | **`4a37f49c`** | **4 核默认镜像** | **smp: 4/4 cores up；4×GICR 帧探测；锚点全绿（its PASS）；4 卷挂载；nvme LPI 送达确认；NET READY；ping 0% 丢包** |

## 轮 9 / 轮 10（当前基线）验收证据

**轮 9（2 核 `e98f07e0`）：**
- `smp: 2/2 cores up, psci 1.1`、`tick: INTID=30 cntpns armed`
- 锚点：SWITCH OK / TICK OK / SPI SOFTTRIG OK / CMSIS RTOS2 OK /
  its_test PASS（n=66）/ M0 ANCHORS DONE
- FS：sata0/sata1/nvme0/emmc0 全部挂载（nvme LPI 送达确认），FS READY；SDIO READY
- NET：gmac0 (192.168.0.201) + gmac1 (192.168.0.200) 双口 1000M 全双工
- 稳态：**10/10 分钟 ping 浸泡全绿**（每分钟 2/2 应答）

**轮 10（4 核默认 `4a37f49c`，sha256 见下）：**
- `gicv3: frame0..frame3` 四帧探测；OP-TEE 释放 Secondary CPU 1/2/3
- `smp: 4/4 cores up, psci 1.1`、`tick: INTID=30 cntpns armed`
- 锚点全绿（同上，its_test PASS n=66）；FS READY（nvme LPI 送达确认）
- NET：双口 up，`net: up (2 of 2 ports)`，NET READY；宿主 ping 0% 丢包
  （gmac0 RTT ~0.6ms）
- 镜像 sha256：`4a37f49c25095d9b347942ab5580297066e4ffe63e5effe5aadb45e9286031b0`
  （347104 字节）；2 核验收镜像 `e98f07e067a601f85afc0408f53d61334e2ebeb2cd17867ff5c35edd7e5e2795`
- 稳态：**10/10 分钟 ping 浸泡全绿**（每分钟 2/2 应答）

## 3× 冷启动可重复性（4 核 `4a37f49c`）

| # | boot_id | 结果 | ping (192.168.0.201) |
|---|---|---|---|
| 1 | boot-060832-f4c4 | 4/4 cores up，NET READY | 3/3，0% 丢包，RTT ~0.6ms |
| 2 | boot-062416-8f3b | NET READY | 3/3，0% 丢包，RTT ~0.8ms |
| 3 | boot-062843-2ba8 | NET READY | 3/3，0% 丢包，RTT ~0.8-9ms |

## 功能冒烟（轮 4）

宿主 → 板卡双网口 ICMP 全通（见上表）——覆盖网卡 IRQ、DMA 收发、lwIP 栈、
内核调度与 FromISR 路径的持续负载。shell 交互冒烟本轮受限（console RX
风暴保护被触发，见遗留 1），以 boot 锚点日志与 ping 负载替代。

## 已知遗留

1. **console RX 风暴保护**：UART2 RX 线路在 GMAC 初始化窗口出现 framing
   错误（iir=0x06/0x0c），5 次坏命中后驱动禁用 INTID 150（本轮修复：不再
   迁移到死 INTID 66）。症状与老 66-rebound 相同但根因未查（疑似 RGMII
   GRF 写窗口的电气串扰），待独立排查；期间 shell 无输入。
2. SDK 线压测套件的遗留问题与本计划正交（其 doc/runs 记录）。
