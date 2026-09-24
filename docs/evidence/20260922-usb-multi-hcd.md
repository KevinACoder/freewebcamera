# 2026-09-22 — CherryUSB 通用 HCD ops 层：四总线同镜像（EHCI×2 + xHCI×2）

**状态：全部完成——编译门禁 + 板验（5 轮冷启动）均通过，四总线清单 ×3 复核一致。**

## 目标

单镜像同时使能两条 HCD：EHCI0/1（`0xFD800000`/`0xFD880000`，INTID 162/165）+
xHCI（DWC3 `0xFD000000`，INTID 202；DWC3 otg-as-host `0xFCC00000`，INTID 201），
为同时枚举四个 USB WiFi 模块铺路（验收口径 = M6 枚举，SCSI 模式切换留 M7）：

| 模块 | 上电 VID:PID | 总线口位 |
|---|---|---|
| RTL8188EUS | `0bda:8179` | bus1 (EHCI1) CH334P hub port1 |
| AIC8800D80 | `1111:1111`（U 盘态） | bus1 (EHCI1) hub port2 |
| rtw88 8821CU | `0bda:1a2b`（CD-ROM 态） | bus2 (xHCI fd000000) 直连 |
| RTL8851BU | `0bda:b851` | bus3 (xHCI fcc00000) 直连 —— RTOS 侧首枚 |

（口位出处：`~/.agents/skills/rk3568-board-lab/references/board-hardware.md` L72-88、
工作区 `docs/DESIGN.md` L697-699，Linux 线 2026-09-13 实测。）

## 上游考证（纠正"上游支持 EHCI+xHCI 共存"的记忆）

上游 master `0e40349b` **不支持**两种 HCD 同镜像：

1. `port/xhci/` 只有 Phytium 闭源静态库（`libxhci_a64.a`，仅 Phytium 平台，
   源码需邮件索取），无开源 xHCI port；
2. 架构上也不允许：`usb_hc_init/usb_hc_deinit/usbh_submit_urb/usbh_kill_urb/
   usbh_roothub_control/USBH_IRQHandler` 六符号是各 HCD 同名定义的移植契约
   （`common/usb_hc.h:66-112`），core（`usbh_control_transfer`、`usbh_kill_urb`）
   与 hub 线程（`usb_hc_init`）裸调用，无任何按 bus 路由的中央层；
3. 上游真正支持的只是**同一 HCD 多实例**：`CONFIG_USBHOST_MAX_BUS` +
   `USBH_IRQHandler(busid)` 按实例传参（T113 双 EHCI 为官方样例），Kconfig 的
   host IP 是单选 choice。

## 方案：通用 HCD ops（vendored 小改，上游 patch 形态）

单实例默认**零变化**（原契约原样保留，兼容上游既有 port），`CONFIG_USBHOST_MULTI_HCD`
门控多实例：

- `common/usb_hc.h`：新增 `struct usbh_hcd_ops`（`hc_init/hc_deinit/get_frame_number/
  roothub_control/submit_urb/kill_urb/irq(busid)` + `driver_name`）与
  `usbh_hcd_register(busid, ops)` 声明；
- `core/usbh_core.h`：`struct usbh_bus` 增 `const struct usbh_hcd_ops *hcd_ops`；
- `core/usbh_core.c`：multi 宏下新增 `usbh_hcd_register` 与六分发器（统一名保留，
  core/hub 调用点零改动自动路由）；不开宏 = core 字节不变；
- 两个 port（vendored `usb_hc_ehci.c` + 自有 `usb_hc_xhci_netbsd.c`）对称改造：
  include 之后加宏改名块（7 个入口 `usbh_ehci_*`/`usbh_xhci_*`，含 `USBH_IRQHandler`
  →`usbh_ehci_irq`/`usbh_xhci_irq`），配显式新名原型（`usb_hc.h` 的声明在 include
  时已按旧名固定，宏改写不了它们——首轮编译即暴露 implicit declaration，已补原型），
  文件尾导出 `usbh_ehci_ops`/`usbh_xhci_ops`。**函数体零改动**。

改名宏必须放在 include 之后：`usbh_core.h:269` 有 `#ifdef USBH_IRQHandler #error`
守卫（上游禁止把 IRQ 名当宏），include 时宏尚不存在，检查先通过。

## 适配层与平台

- busid 布局（`usb_board.h`）：0/1=EHCI、2/3=xHCI；`USBH_BUS_IS_EHCI/XHCI`、
  `USBH_XHCI_BASE/IRQ(id)`；`CONFIG_USBHOST_MAX_BUS` 2→4；
- `usbh_glue.c`：`usb_hc_low_level_init` 按 busid 分发（EHCI→usb2phy1 域+arm IRQ；
  xHCI→`usbh_xhci_low_level_init`）；multi 下新增强符号 `usb_hc_low_level_deinit`
  分发；EHCI trampoline 经 `usbh_ehci_ops.irq(busid)`（不开宏仍直调 `USBH_IRQHandler`）；
- `usbh_xhci_glue.c`：重写为 xHCI 专用钩子——per-instance `s_xhci_regs_alive[2]`、
  `usbh_xhci_opbase/hc_running/nports/portsc` 全部按 busid 参数化、IRQ trampoline ×2
  （202/201）、low_level 两钩子改名供分发；dcache 三件套移除（`usbh_glue.c` 单份）；
- `usbh_adapter.c`：`usbh_bus_start` 每 bus `usbh_initialize` 后立即
  `usbh_hcd_register`（hub 线程跑 `usb_hc_init` 前生效）；xHCI 看门狗按
  `USBH_XHCI0_BUSID+i` 轮询两实例；KI-006 kick 保持 EHCI-only；
- `usbh_cmds.c`：`usbh list` bus 标签查 `bus->hcd_ops->driver_name`，基址取
  `bus->hcd.reg_base`；
- `usbh_platform.c`：usb3otg 域拆「域 once（clkgate con10 / SRST con9+con14 脉冲 /
  usb2phy0 GRF 三写）/ per-instance（DWC3 寄存器段抽 `usbh_rk3568_dwc3_host_init(base)`,
  `s_usb3otg_core_done[2]`）」，签名加 `instance`。

## S2 单实例全局变量排查清单（多实例串扰审计）

| 文件 | 全局量 | 判定 | 处置 |
|---|---|---|---|
| usb_hc_ehci.c | `g_ehci_hcd`/`ehci_qh_pool`/`ehci_qtd_pool`/`g_async_qh_head`/`g_periodic_qh_head`/`g_framelist` | ✓ 全 `[CONFIG_USBHOST_MAX_BUS]` 索引 | 无需改 |
| usb_hc_xhci_netbsd.c | `g_xhci[MAX_BUS]`；`struct xhci_hcd` 内环/ctx/devs 全实例化 | ✓ | 无需改 |
| usb_hc_xhci_netbsd.c | `g_xhci_port_evt_seq`（单实例 ISR 递增） | ✗ 串扰 | **挪进 `struct xhci_hcd.port_evt_seq`**，ISR `hcd->`递增，读取器按 busid |
| usbh_xhci_glue.c | `s_xhci_regs_alive`（单 bool） | ✗ 串扰 | **改 `s_xhci_regs_alive[USBH_XHCI_NUM]`** |
| usbh_xhci_glue.c | `usbh_xhci_opbase/nports/portsc` 硬编码 `USBH_XHCI0_BASE` | ✗ 串扰 | **全部按 busid 参数化** |
| usbh_platform.c | `s_usb_bus_domain_done`/`s_usb2phy1_domain_done`/`s_usb3otg_domain_done` | ✓ 域 once | 保留 |
| usbh_platform.c | `s_usb3otg_core_done` + DWC3 寄存器段 | ✗ 需 per-instance | **新增 `s_usb3otg_core_done[2]` + `usbh_rk3568_dwc3_host_init(base)` 按实例** |
| usbh_glue.c | `s_ehci_irq`/`s_ehci_isr` | ✓ 按 busid 索引 | 无需改 |
| usbh_glue.c | `usb_dcache_clean/invalidate/flush` | ✓ 真全局单份 | multi 下唯一份（xhci glue 移除） |
| usbh_adapter.c | `usbh_console_printf` static buf | ✓ 临界区保护，跨总线共享安全 | 保留 |
| usbh_adapter.c | 看门狗 `last_seq[]`/`seeded` | ✓ 已按 `USBH_XHCI_NUM` 维度 | busid 修正为 `USBH_XHCI0_BUSID+i` |
| usbh_core.c（vendored） | `ep0_request_buffer`/`g_setup_buffer`/`g_usbhost_bus` | ✓ 全 `[MAX_BUS]` | 无需改 |

## Makefile

- **修既有 bug**：XHCI 开关块只改 FreeRTOS 分支，ThreadX 分支（原 :452/:458）整体
  重赋值把 EHCI 无条件编回——`XHCI=1` + 默认 THREADX=1 不自洽。HCD 选择块移到两条
  内核线之后，一块管两线；
- 语义：默认 = 四总线 multi-HCD（`-DCONFIG_USBHOST_MULTI_HCD=1`）；
  `make EHCI_ONLY=1` = 旧单 HCD 契约形态（M6 主线，无分发层，回归对照）；
  旧 `XHCI=1` 互斥形态退役；BUILD stamp 记 `SMP_CORES EHCI_ONLY THREADX`。

## 编译门禁（2026-09-22，全部构建通过/未上板）

| 形态 | 内核 | 结果 | 镜像 sha256 |
|---|---|---|---|
| 默认四总线 | ThreadX SMP=4 主线 | ✓ 链接通过，无新增 warning | `502184f268d4…`（401560 B） |
| 默认四总线 | FreeRTOS =1 | ✓ | `0b998d7772f7…`（393344 B） |
| EHCI_ONLY=1 | ThreadX SMP=4 | ✓（旧契约：无 multi 宏、`USBH_IRQHandler` 直调） | `76738b8df5f6…` |
| EHCI_ONLY=1 | FreeRTOS =1 | ✓ | `e20241ee798a…` |

- `tools/cleanroom-scan.sh` **PASS**（vendor trace/filename/copyleft 三项 ok；sdmmc
  四文件为 D20 whole-file-port 正式豁免）。注：AGENTS.md 的裸扫描命令不带豁免清单，
  会命中上述豁免文件——以脚本为准；
- `make k4`（K4 OK）+ `make gates`（check-deps 四项 ok）**PASS**；
- 新增 warning 仅首轮的 implicit declaration（原型缺失），补原型后清零；EHCI 固有
  pointer-to-int-cast 警告与本次改动无关。

## 待板验（等通知，oslab 纪律全程）

分阶段逐层加设备（先易后难，每阶段冷启动 + `usbh list` 对拍）：

1. **ops 路径等价性**：默认镜像仅看 bus0/1——双 hub `1a86:8091` claimed +
   `046d:0990` + `0bda:8179` + `1111:1111`，与 M6-A 清单一致；
2. **+bus2**：`usbh: bus2 … no-driver 0bda:1a2b`，与 M6-B（D38 `d983f844`）对拍；
3. **+bus3（首枚）**：`0bda:b851`——fcc00000 OTG-as-host 在 RTOS 侧从未跑过，
   这是最大不确定点：GSNPSID 读零/端口不动按 KI-012 同法对照 Linux dts/NetBSD fdt
   排查（usb2phy0-otg lane 的 suspend release 已在域序列里）；预插不枚举按 KI-006
   热插拔口径；
4. 四总线全清单 ×3 冷启动；`EHCI_ONLY=1` 旧形态 1 轮回归（M6-A 清单不退化）；
   FreeRTOS 线 1 轮快速板验。

## 风险与遗留

- fcc00000 OTG-as-host 序列按已证fd000000 序列参数化，但 OTG 实例可能需要额外
  GRF/SYS_GRF 强 host 设置（Linux dts 的 otg 口 dr_mode 处理）——板验阶段核实；
- `EHCI_ONLY=1` 两线仅编译门禁（链接零错），未上板——旧形态行为回归靠板验第 4 步；
- sdmmc 适配器的厂商痕迹（`sdmmc_host_dwmmc.c` 等四文件）是 D20 登记豁免，
  AGENTS.md 裸扫描命令与脚本口径不一致——文档口径以脚本为准（另行知会）。
## 板验（2026-09-22 当日执行，oslab 纪律全程）

### R1（b7362ef 镜像 502184f2）：xHCI 两路成功，EHCI 两路枚举死

首次上板：bus2（0bda:1a2b）与 **bus3（0bda:b851，fcc00000 首次 RTOS 枚举）** 正常，
usbh list 四 bus 标签正确；但 **bus0/bus1 树为空**——hub 线程的 INIT/kick/wake 都
打了，之后零输出（无 claimed 也无 ERROR），251s 不恢复。另见偶发 irq: spurious
（89s/173s/239s/245s）。

**对照实验**：EHCI_ONLY=1 旧契约形态（76738b8d）同板 EHCI 枚举完全正常（M6-A 清单
全出）→ 问题锁定 multi 路径。**根因（两个叠加缺陷）**：

1. **域 bring-up 并发**：SMP=4 下四个 hub 线程同时跑 usb_hc_init，EHCI 的 usb2phy1
   域与 xHCI 的 usb3otg 域**并发执行**——后者也打 con14 SRST 脉冲（H_USB2HOST0/1，
   其注释 "in this image it bounces nothing" 在 EHCI-only 时代成立，四总线时代不再
   成立）。时序实测：EHCI init+kick 在 0.21-0.33s 完成，xHCI 域的 con14 脉冲 ~0.4s
   落下——把已初始化的两个 EHCI 硬件复位砸烂：无中断、无枚举、**无任何报错**
   （roothub 读回复位值）。once-guard 的 check-then-set 本身在 SMP 上也是竞态。
2. **hcd_ops 注册时序**（修复尝试中暴露）：把 usbh_hcd_register 提到 usbh_initialize
   之前后炸 fatal: synchronous exception ESR=0x02000000 FAR=0 ——usbh_bus_init 的
   memset(bus,0,...) 把先注册的 hcd_ops 清零，hub 线程分发器解引用 NULL 从地址 0
   取指。

**修复（提交 82c13cd）**：

- usb_start() 在创建任何 hub 线程**之前**，任务上下文串行跑完全部 USB 域
  （usb2phy1 → usb3otg instance0/1）：所有 SRST 脉冲先于所有控制器 init 落地，
  hub 线程里的域调用退化为 no-op；
- usbh_bus_init 跨 memset 保留 hcd_ops（register 先于 initialize 成为合法契约，
  SMP 竞态同时消除）；
- EHCI_ONLY 的启动循环止于 USBH_EHCI_NUM（消除 bus2/3 假 FAIL）。

### R1'（修复版 d7fe49e4）：四总线全绿

usbh list -t（251s 时）：

```
bus0: ehci @fd800000
  port 1: 1a86:8091 high hub (hub)
    port 4: 046d:0990 high dev
bus1: ehci @fd880000
  port 1: 1a86:8091 high hub (hub)
    port 1: 0bda:8179 high dev
    port 2: 1111:1111 high dev
bus2: xhci @fd000000
  port 1: 0bda:1a2b high dev
bus3: xhci @fcc00000
  port 1: 0bda:b851 high dev
```

**四个 USB WiFi 模块 + 摄像头全部同镜像挂树**；M0 锚点全过（its PASS、M0 ANCHORS
DONE）、四盘自动挂载、net 双口 up、零断言。启动期一次 irq: spurious（1.219s），此后
251s 观察未复发（EHCI_ONLY 形态与其余轮次零 spurious，与多 HCD 无稳定相关，留观察）。

### R2/R3（d7fe49e4）：冷启动复核

枚举清单与 R1' 逐行一致（事件行时间戳同量级）；R2/R3 零 spurious 零断言。
**×3 冷启动达成**。

### R4'（EHCI_ONLY 修复版 9a028b32）：旧契约形态回归

M6-A 清单完整复现（双 hub claimed + 046d:0990 + 0bda:8179 + 1111:1111），启动止于
usb: READY（无 bus2/3 噪声），零退化。

### R5（FreeRTOS 四总线 ca313da2）：对照线同绿

同一套 multi-HCD 代码在 FreeRTOS =1 下四清单完整、锚点全过——分发层与内核无关。

### 镜像台账（全部 sha256 对拍 + 板上 crc32 复核过）

| 镜像 | sha256 | 板验 |
|---|---|---|
| 四总线 ThreadX 主线（修复后，现 TFTP 待命） | d7fe49e4… | R1'/R2/R3 全绿 |
| 四总线 ThreadX（修复前） | 502184f2… | R1（暴露缺陷） |
| EHCI_ONLY ThreadX（修复后） | 9a028b32… | R4' 全绿 |
| EHCI_ONLY ThreadX（修复前） | 76738b8d… | R4 对照（EHCI 正常） |
| 四总线 FreeRTOS（修复后） | ca313da2… | R5 全绿 |

收尾：板已 power_off 且 power_status 复核 power_on=false；TFTP 根已恢复 d7fe49e4
待命镜像。

