# Issue：SMP 4 核 shell 无响应 + 控制台 RX 被启动噪声启发式禁用

- 记录日期：2026-09-19（M6-A 验收期间发现，**基线即有**，与 M6-A 代码无关）
- 状态：待闭环（用户指示先关 SMP 推进 M6-A，本问题后续解决）

## 症状 A：4 核主镜像 NET READY 后 shell 无响应

- 环境：`make all`（SMP_CORES=4）主镜像，含 M6-A 之前的基线。
- 对照实验：`git stash -u` 回到基线 `24a69b6`（D34 证据提交，昨日全绿），重建部署
  `67e719ff…`（与实验前 TFTP 根中的"最后已知好镜像"哈希一致），**两次冷启动均在
  NET READY 后对 shell 输入零回显零响应**（`uptime`/`version` 均无输出，无 flood）。
- 结论：shell 无响应非 M6-A 引入；4 核线在"启动锚点全绿"之后存在静默冻结/输入
  通路失效，且当日 100% 复现（与 freertos-smp 线历史的"间歇性冻结"同族或同根）。
- 处置：M6-A 镜像改 `SMP_CORES=1`（单核为 M0–M4 验收基线形态）；SMP 闭环后
  M6 收口时回归 4 核。

## 症状 B：单核下控制台 RX 被启发式禁用（今日稳定复现）

- 环境：SMP_CORES=1 镜像（含 M6-A 与基线）。
- 串口日志在调度器启动前后连续出现 3 次：
  `[uart] rx irq entry: intid=150 iir=00000007 lsr=00000000`
  （IIR=线状态中断、LSR=0 无任何状态位），
  随后 `drivers/uart_ns16550.c` 的风暴启发式打印
  `[uart] line asserts without RX data; console RX disabled on intid 150`
  并**永久关闭 RX 中断**——此后所有 shell 输入（包括 M0 遗留的"首条命令吞没"
  预热 workaround）都不再被接收。
- 依据：iir=7 且 lsr=0 组合更像幻影线状态（如 U-Boot→应用交界的线路毛刺/波特率
  切换残响），而非真实错误；启发式一刀切禁用过于激进。
- 待查方向：①启动窗口内三次幻影 line-assert 的电气/时序来源（是否与 oslab
  串口工具在 go 前后的行为相关）；②启发式改为"掩蔽+延迟重臂"或提高阈值；
  ③`uartint` 运行期换线/重臂能否作为恢复手段（当前 shell 已死，无法入口）。
- 复现：今日 2026-09-19 全部 ≥6 次冷启动（4 核基线 ×2、单核 M6-A ×4+）均出现。

## 对 M6-A 验收的影响

USB 枚举事件为 boot 期自动打印（hub 线程事件驱动，不依赖 shell），验收证据完整；
`usbh list -t` 等交互命令验证推迟到本 issue 闭环后。
