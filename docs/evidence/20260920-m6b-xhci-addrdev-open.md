# M6-B：xHCI(fd000000) 枚举段 —— 通到 Address Device，Parameter Error 17 open

日期：2026-09-20。镜像 `make SMP_CORES=1 XHCI=1`（382816 B，sha256 `236696bb…983e`）。
提交：xHCI 移植 + 修复（本文件同提交）。**口径：枚举段未验收——ADDRDEV-17 open。**

## 1. 各 OS xHCI 对比（本板，双层 USB3 座 = fcc00000 / fd000000，INTID 201/202，HS-only）

| OS | 栈 | 平台初始化归属 | DWC3 quirk | 状态 |
|---|---|---|---|---|
| Linux 6.1.115 | dwc3-of-simple + inno-usb2phy | DT+驱动全包 | dts 6-7 quirks + GUCTL1 bit26(max-speed=HS) | 真值（regdump 在 wiki KI-012：GUCTL1=0x0404018a） |
| U-Boot | dwc3-generic + xhci-dwc3 | 全包；preboot=`usb start; scsi scan; pci enum` | core.c bit26 + GUSB2PHYCFG 3 quirks | 通——继承模型的状态来源 |
| NetBSD 11 | dwc3_fdt(=xhci_softc) + 本地 rk_usb2phy | OS 侧全套（PD_PIPE/CRU/GRF 0x0c00,0x1d2/GPIO3） | 6 quirks + bit26；parkmode/tx-ipgap A/B 无效已删 | 2026-09-03 通 |
| FreeBSD 15.1 | rk_dwc3 + snps_dwc3 + xhci | DT + rk_usb2phy | 7 dts quirks + bit26 | 2026-09-01 通（KI-007/012） |
| standalone SDK | CherryUSB `port/xhci/dwc3-rk3568`（lab 自有，Apache-2.0） | glue 代码全包（含 GCTL CORESOFTRESET + GUSB2PHYCFG=0x1） | bit26 恒置、GUSB2PHYCFG0=0x1、禁 PHYSOFTRST | 2026-09-05 四口 PASS |
| RTEMS/libbsd | 同 FreeBSD 血统 | 自己的 glue | 当时缺 bit26 → 卡 Polling | 待复测 |
| **freewebcamera（本线）** | 同 standalone 血统（D20 整文件，`cc3acfd6`） | **D36：U-Boot preboot `usb start` 拥有平台序列，OS 只读继承** | 无 | **枚举通到 ADDRDEV，17 open** |

上游 CherryUSB 的 `port/xhci/` 只有 Phytium 闭源静态库（排除）；`dwc3-rk3568` 是 lab 自有端口，
经 imports.md 登记引入。

## 2. 本轮已修复（均有板验证据）

1. **glue `usbh_xhci_nports()` 读错寄存器（首启全静默的根因）**：原读
   `opbase+0x04`（=USBSTS，bits[31:24]=0）当作 HCSPARAMS1 → roothub.nports=0
   → hub 线程端口循环零次 → 枚举从未发起。改为 capbase 读。修后 hub 线程
   立即处理端口事件（`Port change:0x02`）。
2. **roothub ClearFeature 读-改-写回写 PED**：PORTSC.PED 是 RW1CS，把快照里的
   PED=1 写回 = 禁用端口（实测端口掉到 PED=0/PLS=7）。改为只写目标 W1C 位+PP。
   修后 ADDRDEV 时刻端口维持 PED=1/PLS=U0/HS（PORTSC=0x00000e03）。
3. **热插拔看门狗**（D36 遗留）：xHCI ISR 只置 intbuf 不做 mq 唤醒；vendored
   的 roothub_intbuf 永不清零、不能做唤醒依据 → 驱动加 `g_xhci_port_evt_seq`
   事件序号，usb_start 任务 100ms 轮询唤醒。
4. **scratchpad sp_array/sp_buf dcache flush**（u-boot 同款；dcbaa[0] 的 flush
   不覆盖 sp_array 内容）、**CSZ=1 断言**（上下文 64B 布局的实测依据落进代码）、
   **ep0 MPS=64 + u-boot 真值注释**（U-Boot 对 HS 用 64，"USB core guesses at a
   64-byte max packet"；8/16/32/64 均合法，与本 bug 无关）、调试计数器/周期
   dump 清理、usbh_xhci_post_init 缩进。

## 3. ADDRDEV Parameter Error 17 —— open（8+ 轮二分结论）

现象：`New high-speed device ... connected` 后，首个控制传输（Get Descriptor(8)）
触发 BSR=1 Address Device，完成码 17（Parameter Error），slot 1/2/3 重试全败。

**已排除（逐项实测，不是猜测）**：
- 输入上下文参数：add=0x3、slot(0x08300000/0x00010000)、ep0(MPS64/cerr3/ctrl/
  deq|cycle/avg8)——与 U-Boot xhci_setup_addressable_virt_dev 逐字段对平；
- DDR 数据正确性：ADDRDEV 失败后 ivac 回读 input_ctx 前缀 48 u32 逐字节正确
  （flush 链路无问题）；
- scratchpad：sp_array/sp_buf flush 后仍 17；
- GUSB2PHYCFG：0x00101408（U-Boot 遗留）与 0x00000001（lab 线实测值）都 17；
- GCTL 级软复位 + 重配（1ms 与 100ms×2 两版）：仍 17；且 100ms 版**随后把
  HCRST/RUN 打超时**（继承态重序列有害，与 09-19 教训一致，已回滚）；
- 端口状态：PED=1/PLS=U0/speed=HS（0x00000e03）下仍 17；
- GUCTL=0x02000010 / GUCTL1=0x0404018a（含 bit26）= Linux 工作态；

**关键对照**：同一驱动在 lab 线自初始化环境（standalone/os-freertos 树，glue 全套
平台序列）2026-09-05 PASS；U-Boot 在同控制器同设备也成功。**继承态 100% 失败**。
`hcs1` 观察项：继承态读 0x01000140（1 port/16 intrs），U-Boot init 期读
0x2000140（2 ports/32 intrs）——同一 RO 寄存器两值，成因未明（记观察项）。

**下一步候选（按优先级）**：
1. U-Boot 逐 TRB/逐寄存器重放（JTAG 或 u-boot 命令化）找出首个分叉点；
2. 试 addr=非 0 的 Address Device（区分 BSR 路径特有）；
3. 对照 U-Boot xhci.c 的 Enable Slot→ADDRDEV 间是否有遗漏的 xHCI 写
   （CONFIG/USBCMD/DCBAAP 时序）；
4. 若需彻底对齐 lab 环境：恢复 glue 平台序列必须**同时恢复其全部前置**
   （09-19 教训：缺前置的重放会把活控制器打死）——需要 D36 修订裁决。

## 4. 门禁与回归

- `make SMP_CORES=1 XHCI=1 gates` / `make SMP_CORES=1 gates`：PASS（k4 +
  cleanroom-scan + check-deps）。
- EHCI 形态上板回归 1 轮：M0 八锚点 + 双 EHCI 枚举（CH334P 1a86:8091 hub、
  SetAddress、hub class 加载）与 M6-A 一致，无退化。
- 坑（本轮二次踩实）：`make deploy` 不带与构建一致的变量会触发 stamp 失配
  全量重编并把**默认(4 核)镜像**部署到 TFTP 根（tasklock 刷屏=4 核基线
  issue A 症状，非 USB 改动）。部署必须 `make <同变量> all deploy`。
- console RX issue B 依旧（每次 boot `console RX disabled on intid 150`），
  验收靠 boot 期自动打印。

## 5. 遗留清单

- **ADDRDEV-17（本 open 的核心）**——见 §3。
- iso（G3/G4）不变；本驱动显式拒绝 iso（M9 按 D10 复核）。
- EHCI+xHCI 单镜像共存（vendored 栈重复强符号阻塞，M7+M8 同镜像前必须做
  dispatch 层）。
- 0xFCC00000 第二实例（OTG-as-host）。
- usbh_kill_urb/超时恢复、FS/LS 经 TT。
- DBG_LEVEL 保持 USB_DBG_LOG 至枚举验收通过（提交内注释已标注回退义务）。
