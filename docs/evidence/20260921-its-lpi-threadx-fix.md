# 证据：ThreadX 主线 ITS LPI 修复与 NVMe 端到端测通（D43）

日期：2026-09-21
内核：ThreadX SMP =4（`make all` 主线）
镜像：`76ef7f0` 起链（tx_glue LPI 放行保护）→ `abee7fd`（nvme_diag + cmd_nvme）
→ `2fd7238`（板验发现的两缺陷修复）；主线最终冒烟镜像 sha256
`5c649e90…dae803bb`（155744 B）。

## 1. 根因：内核适配层的 IAR INTID 截断

D39 遗留 open 项"ThreadX 镜像 ITS LPI 投递不达（itsdump 存证）"的根因：

- `port/adapters/threadx/tx_glue.c` 的 `tx_irq_handler` 对 ICC_IAR1 读值
  **无条件** `& 0x3ff`。LPI INTID ≥ 8192，`8192 & 0x3ff = 0` 恰好落进
  抢占 SGI（TX_PREEMPT_SGI_INTID=0）的**故意为空**分支——handler 永不执行、
  `test_last_irq` 永不变、its_test 卡在 rung 1。
- FreeRTOS 侧 `port_glue.c` 的 `vApplicationInterruptHandler` 自移植起就有
  `< 8192` 放行保护；ThreadX 移植时丢失。
- 该根因与 20260920 的 itsdump 存证完全吻合：GITS 使能、命令队列排空、
  DTE/ITE 有效、prop=0xb1 使能、**pend=0**——中断确实送达且被 IAR ack
  （ack 即清 pend），只是软件路由错了。

## 2. 修复与 A/B 对拍

修复 = tx_glue 加 `raw < 8192` 放行（与 FreeRTOS 同形）。

同一时段、同一板、同一引导流程的 A/B：

| 镜像 | its 锚点 |
|---|---|
| D42 基线 `14d94fe5`（修复前，字节同 D42 验收镜像） | `its: LPI FAIL`（rung 1，INT dev e000 event 0 not delivered） |
| `76ef7f0` 修复后 ×3 冷启动 | `its: LPI OK (n=66)`（单发 / 关断静默+再使能 / 32 静默+32 再发 / 32 交织，全 rung） |

## 3. NVMe 端到端（MSI-X → ITS → LPI）

全链路驱动（dwc_pcie/rk3568_pcie/dwc_msix/dwc_nvme + gicv3_msi ITS 域）
FreeRTOS M3-A 已板验，但 ThreadX 主线的 fs 适配是 stub、无人探测。
`drivers/nvme_diag.c` + `nvme` shell 命令补上该探测路径（内核无关、只读）。

ThreadX 主线 ×3 冷启动一致：

```
pcie0: firmware link inherited (bus 0-1), link Gen2 x1
its: 01:00.0 got 1 message irq(s) (event 0 -> intid 8192)
msix: 126f:2263 rid 0100, 1 vector(s), table BAR0+0x2000 (16 entries)
nvme: 1 message vector(s), completion intid 8192
nvme0: 126f:2263 at 0xf4300000, ns1: 500118192 blocks of 512 bytes
nvme-diag: ctrl 0 model "QUANXING N200 256GB"
nvme read 0 -> read lba 0 ok, sum 929d77e8c763a98c
nvme0: message interrupts delivered (intid 8192, 2 so far)   ← LPI 端到端证据
```

第二轮冷启动换 `nvme read 0x1000` 复现同一送达行；its_test 再次运行时
LPI slot 分配器正确避开 NVMe 占用的 8192（single INT 落 8193）。

## 4. 板验中发现并同轮修复的两个缺陷（2fd7238）

1. **CNS=1 Identify 带 NSID**：`nvme_identify` 无条件 `nsid=c->nsid`；
   规范要求 controller identify（CNS=1）NSID=0，该盘固件拒绝（status 5）。
   修为按 CNS 区分。此路径 M3-A 从未走过（FatFs 只用缓存 caps）。
2. **首行输入消失**：适配器先 `Control(RX,1)` 后从未 `Receive()`，
   引导 arm 填的是驱动私有 scratch，回调却读客户端 rx_chunk——两内核上
   每次启动后第一行键入变 NUL 消失。修为 Receive 先于 Control（客户端
   缓冲先挂上，rx_start 的 client resume 机制接管）。修后首击命令直通。

## 5. 验收口径

- 今日实测：ThreadX =4 主线 ×3 冷启动 its PASS；NVMe LPI 送达行 ×3；
  基线镜像 A/B FAIL 对拍 ×1；两缺陷修复前后对比 ×1。
- 构建矩阵：make all / freertos / ktest / k4 / gates 全绿，负例
  `THREADX=0 SMP_CORES=4` 报错，cleanroom-scan PASS，check-deps PASS。
- 遗留：无（D39 ⑤ 本项闭环）。
