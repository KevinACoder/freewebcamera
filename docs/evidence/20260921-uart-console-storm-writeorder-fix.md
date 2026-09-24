# 证据：FreeRTOS =1 控制台风暴写序归因与修复（D41 闭环）

日期：2026-09-21
内核：FreeRTOS 单核（`make freertos`，CNTV PPI27 tick）
提交：`49db924`（含修复前的归因轮构建，均在本地验证后落最终形态）

## 1. 归因链（三轮探针，全部 NOTICK 实验构建以压制刷屏）

第 0 轮（本轮之前，见 20260920 存证）已知：pend@arm=1、IER=00、
icfgr=level、ICPENDR 清不掉、~16-19.5 kHz 重入、窗口锁在
shell_start 的 Initialize→PowerControl(FULL)→Control(RX,1)。

### 第 1 轮：窗口四点探针（`uart_console_window_probe`，cherrysh w0-w3）

```
[0.030] w w0-pre-init:  pend=0 ier=00 lcr=03 isen=00000000
[0.036] w w1-post-pwr:  pend=1 ier=00 lcr=03 isen=00000000
[0.042] w w2-post-csh:  pend=1
[0.048] w w3-post-recv: pend=1 ier=01
[0.054] arm: pend=1
[0.061] rx irq entry iir=07 → first spurious → ~16 kHz 风暴
```

结论：pend 0→1 发生在 **Initialize→PowerControl(FULL) 之间**，即
program_uart #2；且 **isen=0 时 pend 已锁存**（电平线断言锁存与 GIC
使能无关）——UART INT 物理线在该窗口被拉高。program_uart #1
（board_main，t≈0.005）后探针 pend=0：同写序不同结果，差异在部件
内部遗留状态。

### 第 2 轮：program_uart 逐写微探针（PU_TRACE，仅第 2 次调用生效）

```
[0.036] pu-ier:  pend=0     ← IER=0x00 写：无害
[0.039] pu-dlab: pend=1     ← LCR=0x80（DLAB=1）写：**就是它**
[0.042..] pu-dll/dlh/lcr/fcr/mcr: pend=1（已触发）
```

## 2. 机理

DLAB=1 时 offset 0 别名 IER→DLL：中断评估器把 DLL 的位当 IER 用。
第二次重编程进窗口时 DLL=13（0x0d，bit0=RX、bit3=MS 使能），而同一
序列早前的 MCR=0x03（DTR/RTS 上升沿）已在 MSR 锁存 change-of-state
位——"RX|MS 使能 + COS 挂起"当即拉高 INT 线，GIC 锁存 INTID 150。
线语义此后被破坏（arm 后 IER 真 RX 使能 + 超时条件 → 线持续断言），
即 ~16 kHz spurious 风暴。第一次编程幸免于进窗口时 DLL=U-Boot 遗留
0、MSR 干净——这解释了风暴为何跟随**重编程**而非引导本身。

## 3. 修复（49db924）

program_uart 值感知幂等：驱动独占这些寄存器，影子寄存器全匹配则
第二次调用**零寄存器写**（DLAB/FCR 危险窗口不再重开），
PowerControl(FULL) 契约由比对保证。同提交：usart_rx_start 先 drain
后开 IER（对齐 usart_receive 安全序）；arm 快照补 GCD ISENABLER 字；
w0-w3 窗口探针保留为常驻诊断。

## 4. 验收

- NOTICK 修复验证轮：w0..w3、arm **全 pend=0**，零风暴行，shell READY。
- 正式带 tick 镜像 ×2 冷启动：引导零风暴行；**首击命令直通**
  （`gicdiag: PMR=f0 RPR=ff rx-down=0`、`rtos: semaphore/queue/mutex OK`），
  防御层零触发——**=1 镜像 shell 首次可用**。
- ThreadX 主线镜像同源码冒烟 ×1：无回归，its PASS + NVMe LPI 送达。
- 20260920 存证中的防御层（armed-off/kick/line-fallback）原样保留，
  作为未来任何线级异常的兜底。

## 5. 口径

今日实测；D41 决策行"写序修复实验"闭环。旧启发式（IIR=0x07 误分类）
与 D40 的"卡 ACTIVE"误判已在 20260920 证据更正，本轮不再重复。
