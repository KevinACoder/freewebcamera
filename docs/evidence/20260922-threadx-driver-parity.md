# ThreadX SMP 对齐 FreeRTOS 全驱动支持（P0–P4，2026-09-22）

## 结论

ThreadX SMP（4 核）主线镜像补齐了 FreeRTOS 对照线的全部驱动支持面。此前
ThreadX 线只有 shell + ITS/LPI + NVMe 诊断（D39/D42/D43），本轮把 periph、
存储（AHCI/NVMe/eMMC 经 FatFs + SDIO 枚举）、网络（双 GMAC + lwIP）、USB
（双 EHCI + CherryUSB hub）四条线全部接线并板验。同时对齐过程暴露并修复了
**共享 GMAC 驱动的 SMP 跨核竞态**（对两条内核线都生效）。

## 提交区间（freewebcamera）

| 提交 | 内容 |
|---|---|
| `3c599e2` | P0 基座：cmsis_os2_threadx 补 `osKernelLock/Unlock/RestoreLock`（映射 `_tx_thread_smp_protect` 全局保护，嵌套深度语义同 FreeRTOS 孪生层）；`port/adapters/threadx/heap.c`（.bss 1 MiB 堆，pvPortMalloc 面）；线程栈改 attr 定长自堆分配；修槽位回收时 per-thread event-flags 泄漏；槽池扩容（线程 16→32、eflags/mutex 8→16、sem 8→32、queue 8→16、timer 8→16、pool 4→8） |
| `532a677` | P1：periph 适配器编入 ThreadX 线 |
| `17bced6` | P2：存储线接线（fatfs_os/sdmmc_osa 去内核头——FreeRTOS 镜像哈希不变证明零语义；FATFS/SDMMC 源组 + 两适配器入线，fs/sdio stub 退役） |
| `0b19a1a` | fix(driver)：dwc_eqos 环形扫描移出中断（SMP 竞态，见下） |
| `c8c7ae0` | P3：网络线（port/adapters/lwip/cmsis/ 的 sys_arch + Makefile 接线） |
| `d0ec55d` | P4：USB 线（usb_osal_threadx.c 适配移植 + CherryUSB EHCI 子集入线，usb stub 退役） |

## 板验（全部冷启动、oslab 纪律）

### ThreadX SMP=4 主线

- **P0+P1（镜像 01c07986）×2 冷启动**：M0 锚点全绿（SWITCH/TICK/SOFTTRIG/
  CMSIS RTOS2 OK、its PASS n=66）；periph 五命令 `i2c scan 0`（2 设备
  0x20/0x40）、`rtc`（秒滴答 +1）、`pwr`（INA3221 三轨）、`temp`（到已知
  TSADC 板级限制，与 FreeRTOS 线一致）、`sfc info`（jedec ef 60 17 对拍）；
  `nvme read 0` LPI 送达 + 读校验。
- **P2（5c91943a）**：引导期四盘自动挂载——`sata0/1 122104 MB`、
  `nvme0 244198 MB`（LPI 送达）、`emmc0 29824 MB`，全 FAT32；8 MiB 文件读回
  校验 emmc **13653 KiB/s**（FreeRTOS 线 14.2 MB/s）、nvme **7968 KiB/s**
  （FreeRTOS 线 7.8 MB/s），fnv1a 跨盘一致（e3472c45）；SDIO RTL8189FTV
  枚举（24c:f179，HS 50 MHz）。
- **P3（5a0a9cde → 0f7acb87 修复后）**：lwIP 2.2.1 双口 1000M full duplex、
  静态 IP；`net dhcp 1 on` 拿到 192.168.0.83（与 U-Boot 租约一致）；修复后
  200+ 秒双口广播流量零断言零丢包（修复前 14 秒即 pbuf 断言挂 tcpip 线程）。
- **P4（a159ca85）**：双 EHCI 总线 init、两个板载 CH334P hub（1a86:8091）
  claimed；`usbh list`：bus0 hub 下 **046d:0990（罗技摄像头，本项目核心
  外设）**、bus1 hub 下 **0bda:8179（Realtek USB 网卡）** + 1111:1111 测试
  设备——与 M6-A 清单一致。

### FreeRTOS 单核回归（dfc09875，eqos 修复后）

冷启动全锚点绿；lwip 双口 1000M up、rx 计数走、零断言零丢包；usb/sdio
READY。修复对单核语义等价（单核 IRQ 天然与持锁线程互斥）。

## 根因记录：dwc_eqos 的 SMP 跨核竞态（0b19a1a）

`eqos_lock` 的设计是"mutex 管线程间、屏蔽本口中断管中断-线程间"——这是
**单核互斥模式**：4 核上另一核已在途的本口中断不受屏蔽约束，IRQ 无锁跑
`eqos_rx_complete/tx_complete` 改共享环形，与核 0 上持 mutex 的线程并发。
lwIP 表现为 `pbuf_free: p->ref > 0` 断言（lwip_arch_assert 按 WFE 驻留，
tcpip 线程死）。修复 = 环形变更全部移入持锁线程上下文：IRQ 只读锁存位、
W1C、举事件；RX 扫描挪进 `GetRxFrameSize`（事件 + 20ms 兜底驱动）、TX 扫描
挪进 `SendFrame` 窗口，看门狗 100ms 兜底不变。投递时延不变（同一中断触发）。

**模式**（任何驱动在 SMP 线上都要过一遍）：单核下"IRQ 直改共享状态 + 线程
用锁"是安全的；SMP 下 IRQ 与线程可在两核并发，中断屏蔽不再构成互斥——
共享状态要么全在锁内线程上下文改，要么锁本身必须 ISR-safe 且跨核。

## 定位差异（本线决策，详见 DESIGN §14）

- **D48 ThreadX 对齐线**：驱动全 CMSIS 化的回报兑现——12 个驱动零改动复用；
  对齐工作全部落在适配层（cmsis_rtos2_threadx 补 API + 各内核专属 osal）；
  lwIP sys_arch 按 cmsis_os2 新写（contrib 的 FreeRTOS 版不共用，FreeRTOS 线
  零改动）；CherryUSB 沿上游 threadx osal 适配移植（`usb_osal_init` 收敛为
  自持池 + 幂等，usb_start 在 THREADX_BUILD 门控下调用）。
- **D49 中断-线程 SMP 互斥模式**：见上；登记为所有中断驱动上 SMP 前的审计项。

## 明确不做（本轮口径）

FreeRTOS 线不改行为（共享修复除外）；不 fdisk/mkfs/写盘；SDIO WiFi 数据面
（对齐口径 = 枚举，与 FreeRTOS 线一致）；ktest 套件 ThreadX 化；xHCI 变体
ThreadX 化；tickless。

## 未验证 / 遗留

- `temp` 的 TSADC 传感器不可读为已知板级限制（M2-A 记录，CON51 EL1 不可写），
  两线一致，非本轮回归。
- smp_test 的 done-flag setter 缺失（D39 open 项）仍在。
- lwIP 双口同网段的路由选择（net_cmds.c 头注释）为 FreeRTOS 线既有行为，
  本轮未变更。
