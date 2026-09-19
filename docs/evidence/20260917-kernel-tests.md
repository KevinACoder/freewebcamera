# 2026-09-17 内核测试子系统（D34）板验证据

官方 FreeRTOS 测试套件（monorepo `f4fcc3b2`，MIT）移植为独立 ktest 镜像。
代码与登记同一提交：`0b6b888 feat(tests)`（IMPORT-INFO.md + docs/imports.md）。

## 镜像与构建

| 变体 | 字节 | sha256 |
|---|---|---|
| ktest =4 | 401016 | `a1657008a6ed34cdb37fa2bcbff90dfeb25297324ba6cbfd2f2628b66243e201` |
| ktest =2 | 400968 | `1756494123fa0a98e6f8e984d23f375b8058125d7928d6c85377a801266c357a` |
| ktest =1 | 393736 | `0028e3f6a527ce8e14d79c17a3e16d3cee00d772301e96f5f044fa0b80a9e0fd` |
| 主镜像 =4（配置超集回归） | 352224 | `67e719ff49b824337c66c07a26110cf4bcfb98126113cc97d5858ef42362ebb8` |
| 主镜像 =1 | 344688 | `fc157aab37489d12…`（构建验证） |

- `make gates` 全绿（cleanroom-scan PASS / check-deps PASS / k4 链接绿）。
- =4 重复构建哈希逐字节一致（构建确定性 + SMP_CORES 标志戳命中验证）。

## 板验轮次（全部 KI-001 冷启动）

| 轮 | 镜像 | boot id | 验收 |
|---|---|---|---|
| 1 | ktest =4 | `boot-091507-5fb8` | 4/4 核 up、tick CNTPNS/30、套件横幅 + 20 套件清单；**153 行监控行（≈12.8 min）全部 `No errors`，零 Error/fatal/assert** |
| 2a | ktest =2 | `boot-092916-c5e8` | 2/2 核；**149 行（≈12.4 min）全 `No errors`** |
| 2b | ktest =1 | `boot-094252-5905` | 单核比较器：tick 正确切 **CNTV/INTID 27**、单 GICR 帧；**10.8 min 全 `No errors`**；SHELL READY |
| 3 | ktest =4 ×3 冷启动 | `boot-091507-5fb8` / `boot-095421-6b13` / `boot-095634-e2b9` | 三次全绿（每次 4/4 核 + 监控流正常 ≥2 min） |
| 4 | 主镜像 =4 | `boot-095916-b030` | 配置超集零回归：SWITCH/TICK/SPI SOFTTRIG/CMSIS RTOS2 OK、its_test PASS（n=66）、M0 ANCHORS DONE、SDIO READY、4 卷挂载 FS READY、双口千兆 NET READY |

覆盖的 v1 套件（20 + IntQueue = 21 个任务级套件，全部由 5s 监控任务以
`xAre*StillRunning` 验活）：sem、countsem、recmutex、blocktim、GenQ（含
abort-delay 扩展）、AbortDelay、PollQ、BlockQ、QPeek、dynamic、death（自杀
压力）、integer、TaskNotify、TaskNotifyArray、EventGroups（含 tick-hook ISR
侧）、QueueOverwrite、QueueSet、QueueSetPolling、TimerDemo、IntSemTest（tick
hook 驱动）、**IntQueue**（双中断源：tick hook 每拍 `xFirstTimerHandler` +
INTID 60 软件触发 SPI @优先级 12 ≈50 Hz `xSecondTimerHandler`，真嵌套）。

## 排除项（开关位已留，理由登记于 tests_config.h）

flop/sp_flop/RegTests（FPU，`-mgeneral-regs-only`）；StreamBuffer/MessageBuffer
三件（官方单核假设 + 参考 SMP 线标注需改）；flash/comtest/crhook/crflash（无
硬件或无收益）。

## 与参考 SMP 线压测的对标

`rk3568_lab/os/freertos-smp/example/smp_pressure_test/` 是同一官方套件的厂商
拷贝，其 doc/runs 记录 **4 核从未跑通**（次核早期 ICC_BPR1 断言 + 厂商标注
flop/recmutex/QueueSet/TimerDemo/MessageBuffer/RegTests 六处疑点）。本仓取上游
main 逐字为准（仅 IntQueue 采纳其两处实质 SMP 适配，其余厂商改动为调试脚手架
不采纳），首日即在 4 核跑出 ≥12 min 零错误的浸泡——其 stage-E 遗留就此闭环。

## 已知遗留 / 新观察

1. **UART2 RX framing 噪声反证**：ktest 镜像**无任何 GMAC/RGMII 活动**，仍在
   启动后数秒内命中同样的 `iir=0x07/0x0c` line-assert 噪声并触发 5 次禁用保护
   （三份 ktest 镜像一致）。此前"RGMII GRF 写窗口电气串扰"假设被削弱；OP-TEE
   副核释放窗口成为新嫌疑。根因排查仍待独立轮次；期间 shell 输入无效。
2. `ktest` shell 命令（实时逐套件状态）已实现但本轮未能交互验证（受 1 的
   RX 禁用影响）；命令体仅调 `ktest_report()`，风险极低。
3. StaticAllocation 套件上游模板本就不由 TestRunner 启动；静态路径由内核静态
   idle/timer 任务（configKERNEL_PROVIDED_STATIC_MEMORY=1）覆盖。
