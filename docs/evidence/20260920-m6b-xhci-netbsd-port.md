# M6-B xHCI（D38）：NetBSD 序列整建制移植，枚举打通

日期：2026-09-20 ｜ 状态：**已闭环**（3× 冷启动验收 + EHCI 回归通过）
镜像：XHCI=1 `d983f844dc71c4678b5530aee2f7b882eb4d94cf442eeeeaf1ba32ec9be2edc9`
（381648B，`make SMP_CORES=1 XHCI=1 all deploy`，宿主/TFTP sha256 对拍一致）
前置：D20 lab 移植枚举卡 ADDRDEV Parameter Error 17（见 `20260920-m6b-xhci-addrdev-open.md`），
用户裁决弃"修继承态"，从 NetBSD 整建制移植。

## 1. 决策依据：为什么是 NetBSD

时间线证据：NetBSD xHCI 2026-09-03 在本板打通（fd000000 枚举 U 盘、HS bulk 饱和）
时 U-Boot preboot 还只有 `scsi scan` —— **USB 域完全由 OS 自举**（rk_usb2phy 域序列 +
dwc3_fdt quirks + xhci.c 全新 init）。D36 的"零写继承"模型没有任何已验证参考；
lab glue 的 CORESOFTRESET 实验失败是因为没有 CRU SRST 脉冲前置、序列与 NetBSD 不同。

**hcs1 之谜结案**（撤销 D37 留下的线索）：0x01000140 vs 0x2000140 按 spec 解码
（slots[7:0]/intrs[18:8]/ports[31:24]）是 1 口（usbhost）vs 2 口（otg）——
两个不同控制器的读数被对比了，非硬件异常。

## 2. 移植内容（提交 `feat(usb): M6-B xHCI via NetBSD port`）

| 文件 | 内容 |
|---|---|
| `xhci/usb_hc_xhci_netbsd.c`（新） | NetBSD netbsd-11 `sys/dev/usb/xhci.c`（rev 1.188.2.4）寄存器序列 → CherryUSB 契约。BSD-2 版权头保留 |
| `xhci/usb_hc_xhci.h`（修订） | TRB 类型按 spec §6.4.1 修正（旧值整体偏 1）；完成码校正；EP ctx 布局修正（avg_trb_len 在 word4）；DWC3 pipe 位；HCSPARAMS1 spec 解码注明 |
| `usbh_platform.c/.h` | 新增 `usbh_rk3568_usb3otg_domain_init()`：rk_usb2phy 域序列（PD_PIPE ensure、con10 门控、con9/con14 SRST 脉冲、con28/29 释放、usb2phy0 GRF 0x0c00/0x1d2/480m、VBUS）+ dwc3_fdt 序列（soft reset、6 quirks、GUCTL1 bit26、DCFG=HS、PRTCAP=host），寄存器值 1:1 |
| `usbh_xhci_glue.c` | D36→D38：`usb_hc_low_level_init` 先跑平台序列再装 IRQ/nports |
| `usb_board.h` | 补 con10/con9、usb2phy0 GRF 常量 |
| `usb_hc_xhci_dwc3_rk3568.c`（删） | D20 lab 移植退役，历史在 git |
| `Makefile` / `usb_config.h` | XHCI 分支换文件名；环尺寸对齐 NetBSD 256；DBG 回退 ERROR |

## 3. 板验修掉的 6 个 bug（14 轮上板二分，全部实测定位）

1. **dwc3 soft reset 少了收尾**：漏 `GCTL.CORESOFTRESET` 清除 → xHCI 孔径读零。
2. **HCIVERSION 非对齐 MMIO**：32 位读打在偏移 2 → alignment fault（ESR 96000021）。
   改为一次对齐读 CAPLENGTH 高半字。
3. **TRB 类型整体偏 1**：`TRB_TYPE_SETUP=1`（应为 2）→ SETUP 被当 NORMAL 执行，
   控制传输永不完成。spec §6.4.1：NORMAL=1/SETUP=2/DATA=3/STATUS=4/LINK=6。
4. **EP ctx 布局错位**：avg_trb_len 写在 word3（TR Dequeue 高 32 位）→ 控制器拿
   `0x8_xxxxxxxx` 找环，静默不执行。正确：word2/3=64 位 dequeue，word4=avg_trb_len。
   板验铁证：输出上下文回读 `ep0 deq_hi=0x00000008`。
5. **BSR=0 地址语义**：本机 DWC3 的 BSR=0 把输入 slot ctx 地址字段当线上 token 的
   新地址（写 2 → 设备还在 0 → 三连 Transaction Error code=4）；写 0 则控制器
   **自动分配地址**（1 起，线上 SET_ADDRESS(分配值) 设备应答），分配值写回输出
   上下文。策略：恒写 0，`assigned usb addr N (state=2)` 以输出为准；
   CherryUSB 的 dev_addr 只是簿记不参与 token。
6. **td first/last_idx 用了批内偏移**：`complete_td` 拿事件绝对下标与 last_idx 比对
   判 status 段 + 终态镜像回 first_idx 槽——偏移值让 9 字节 config 读取的事件被
   误吞（ep=6/dq=6/fi=3 取证实锤）。`ring_put` 存副本时改写绝对下标。

（另有继承态诊断阶段的板验修复——scratchpad 三重 flush、nports 读 cap 帧、
ClearFeature W1C-only、热插拔看门狗——已随 D20 提交在案，本移植保留同款语义。）

## 4. 验收记录

XHCI=1（`d983f844…`）3× 冷启动全绿，逐次串口同口径：
`[USBH] usb3otg domain up: GCTL=0x30c11004 GUCTL1=0x0404018a GUSB2PHYCFG0=0x00101408
GUSB3PIPECTL0=0x11080002` → `[USBH] xHCI: 1 ports, 64 slots, ctxsz=64 pgsz=4096
caplen=32 hcs1=0x01000140` → `[USBH] usb_hc_init done bus0` →
`[USBH] xHCI slot 1 enabled` → `[USBH] assigned usb addr 1 (state=2)` →
`usbh: bus0 hub1 port1 no-driver 0bda:1a2b device (high)`。
M0 八锚点 + SDIO/FS/NET READY 照旧，零 FAULT/ASSERT。
（0bda:1a2b = Realtek WiFi 网卡 CD-ROM 模式，mass-storage 类镜像未编 msc →
no-driver 为预期报告；枚举即验收口径。）

EHCI 形态回归 1 轮（`6920f89e…`）：bus0 hub2 port4 罗技 046d:0990、bus1 hub3
port1 0bda:8179、port2 1111:1111 全枚举，M6-A 行为零退化。

门禁：`make SMP_CORES=1 XHCI=1 gates` 与 `make SMP_CORES=1 gates` 双形态
cleanroom-scan + check-deps PASS。

## 5. 遗留

- iso 等时仍显式拒绝（G3/G4，M9 按 D10 复核）。
- EHCI+xHCI 单镜像 dispatch 层（M7 USB-WiFi 前必须做）。
- FS/LS via TT、hub 深拓扑、kill_urb/超时恢复完善。
- 0xFCC00000 第二实例（OTG 口）。
- CherryUSB 簿记 dev_addr(2) 与硬件分配地址(1) 不同步——纯软件视图差异，
  token 由控制器 slot ctx 决定，无线上影响；M7 接 WiFi 驱动时留意即可。
