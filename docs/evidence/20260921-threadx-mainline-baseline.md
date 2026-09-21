# 基线重构：ThreadX SMP 升主线，FreeRTOS 限单核（D42）

- 记录日期：2026-09-21
- 提交：`d1c71a8`（refactor: 清除 FreeRTOS SMP）→ `59673f8`（feat: 主线切换）→ 本文件
- 决策：DESIGN §14 **D42**（用户指示：ThreadX SMP 为主要适配 OS；FreeRTOS SMP 不再支持；FreeRTOS 单核保留）

## 1. 目标态与实现

| 项 | 之前 | 之后 |
|---|---|---|
| `make all` | FreeRTOS SMP =4 | **ThreadX SMP =4**（THREADX ?= 1） |
| FreeRTOS SMP | D32/D33 交付形态 | **移除**（`THREADX=0 SMP_CORES≠1` 构建报错） |
| FreeRTOS 单核 | `make SMP_CORES=1 all` | **`make freertos` / `freertos-deploy`**（=1） |
| ktest | 继承 SMP_CORES（默认 4） | 钉死 `THREADX=0 SMP_CORES=1`（修掉 =1 内核 × =4 板 tick 源的组合错误） |
| 板层（smp.c/smp_secondary.S/PSCI） | — | **零改动**（ThreadX =4 在用） |
| 部署名 | freertos.bin | 不变（oslab profile 写死；banner 区分内核） |

清除清单（FreeRTOS SMP 物理删除，=1 路径全保留）：
`FreeRTOSConfig.h`（NUMBER_OF_CORES 硬设 1，删 affinity/multi-prio/nesting-in-TCB）、
`port.c`（SMP 数据分支/vPortSmpCoreISRSetup/StartSecondaryCpuUp 调用点/SMP tick
handler/MCS 锁整块/vTaskSwitchContextISR）、`portmacro.h`（SMP 临界区/锁/工具块）、
`portasm_smp.S`→**`portasm.S`**（pxCurrentTCB 标量无条件）、`port_glue.c`（副核真身，
留 wfe park）、cmsis_rtos2 adapter（affinity 忽略/恒资源错）、shell `smp` 命令单核 gate。
vendored 内核与测试套件、NOTICK/风暴诊断设施未动。

## 2. 构建矩阵（全绿）

| 命令 | 产物 | 结果 |
|---|---|---|
| `make all` | build/rk3568-threadx/threadx-smp.bin (154112B) | ✓ |
| `make freertos` | build/rk3568/freertos.bin (379464B) | ✓ |
| `make ktest` | build/rk3568-ktest/freertos-ktest.bin (428608B) | ✓ |
| `make k4` | 内核替换 stub（K4 门） | ✓ OK |
| `make gates` | cleanroom-scan + check-deps | ✓ PASS ×2 |
| `make THREADX=0 SMP_CORES=4 all` | — | ✓ 按设计报错（D42 守卫） |

## 3. 板验（oslab 纪律）

### ThreadX =4 主线（镜像 14d94fe5 —— 与重构前一轮完全同哈希：改动全在被编译掉的分支与 FreeRTOS 侧，主线零影响）
- **boot-183012-d434**：锚点全绿（`shell: READY` 0.093、`app: SWITCH/TICK/SPI SOFTTRIG OK`、`CMSIS RTOS2 OK`、`M0 ANCHORS DONE` 0.867；`its: LPI FAIL` 为既有 open 项）。shell 交互：`version`/`uptime` 逐字符回显正常，`uptime: 79.698 s (cntvct); tick 79560 at 1000 Hz = 79.560 s` 双时钟一致；`smp` 命令 4 任务分别在 core 0/1/2/3 首跑、20000 采样全部完成（`TIMEOUT, 4/4 tasks reported` 为既有 app 侧 done-flag 未置位，D39 已登记，非内核问题）。
- **boot-183321-e431**：锚点全绿复现；shell `gicdiag: PMR=f0 RPR=ff console-intid=150 rx-down=0` 正常响应。

### FreeRTOS =1（镜像 76237e1e）
- 风暴如 D41 所记（open）：`arm: pend=1` 后 ~19.5kHz 重投递；**防御层受控**：arm-off → ~0.51s kick 周期规整（13.735/14.311/14.888…），系统未死。锚点段被风暴刷出串口环；重构前同代码路径（boot-085831，7c90d024）锚点全绿有案，本轮防御循环行为与其一致（周期/增长率）= 重构后 =1 可运行、无回归。
- shell 在 =1 因 open 风暴不可用（写序修复 = D41 遗留独立线）。

## 4. 口径

- **今日实测**：§3 全部；构建矩阵全绿。
- **未验证/open**：=1 风暴写序修复（D41）；ThreadX ITS LPI（D39 既有）；ktest =1 板上浸泡（本轮仅构建验证，套件 =1 历史 10.8min 浸泡记录在案）。
