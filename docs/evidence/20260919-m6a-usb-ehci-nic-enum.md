# M6-A 板验证据：CherryUSB（upstream master）+ EHCI 双路 + 面板 USB 设备枚举

- 日期：2026-09-19
- 镜像：`freertos.bin`（**SMP_CORES=1**，见下方"基线问题"），sha256
  `0d3ea14b8a642a9df414c6917466f679e42810390700360ea5d294032d0faa76`（379424 B）
- 上游锁定：cherry-embedded/CherryUSB master `0e40349b4b5615f019f9d34c1ac92a7b23433a94`
  （2026-09-18；fresh clone 拷贝，字节一致）
- 门禁：`make SMP_CORES=1 gates` = k4 + cleanroom-scan PASS + check-deps PASS
- 决策：DESIGN §14 **D35**；登记 imports.md §1/§1.1/§4/§4.1 与 IMPORT-INFO.md

## 验收判据（M6-A 口径 = 枚举，不写设备驱动）

`usbh startall`（本轮以 boot 期 `usb_start()` 自动执行）后，两路 EHCI 各自经
CH334P hub 枚举出面板设备，`no-driver` 即"枚举成功、无类驱动认领"的预期形态。

## 冷启动 ×3 逐行一致（boot-125811 / boot-130131 / boot-130306）

每轮关键序列（串口原文）：

```
usbh: bus0 hc init
usbh: bus0 port1 portsc=00001801 ppc=1        ← CCS=1 设备在位
usbh: bus0 port1 kicked, now 00001801         ← KI-006 PP 断电/上电
usbh: bus0 wake hub thread (intbuf=0002)
usbh: bus1 hc init
usbh: bus1 port1 portsc=00001801 ppc=1
usbh: bus1 port1 kicked, now 00001801
usbh: bus1 wake hub thread (intbuf=0002)
USB READY
usbh: bus0 hub1 port1 claimed 1a86:8091 hub (high)      ← CH334P（bus0）
usbh: bus1 hub1 port1 claimed 1a86:8091 hub (high)      ← CH334P（bus1）
usbh: bus1 hub3 port1 no-driver 0bda:8179 device (high) ← RTL8188EUS 无线网卡
usbh: bus0 hub2 port4 no-driver 046d:0990 device (high) ← Logitech UVC 摄像头
                                                        （Class 0x0e 视频 ×2 + 0x01 音频 ×2，4 接口）
usbh: bus1 hub3 port2 no-driver 1111:1111 device (high) ← MSC 类设备（Class 0x08/06/50）
SWITCH OK / TICK OK / SPI SOFTTRIG OK / CMSIS RTOS2 OK
ITS LPI OK (n=66) / M0 ANCHORS DONE
SDIO READY（24c:f179） / FS READY（sata0/1+nvme0+emmc0 全挂载） / NET READY（双网口 link up）
```

- 三轮全部：M0 八锚点全绿、ITS PASS、四盘挂载、双网口 up，**零 FAULT/ASSERT**。
- `portsc=00001801` = CCS+PP；kick 后仍 CCS=1 证明设备持续在位，枚举由 hub 线程完成。
- 046d:0990（QuickCam Pro 9000，UVC）在本轮口径下只枚举不驱动，为 M8 提前验明可用。

## 调试过程中钉死的板级事实（全部已固化进适配层）

1. **PMUCRU clkgate 数据位极性**：`CLKGATE_CON2` 低位 **1=关钟、0=开钟**。
   初版误写 `GRF_WR(0x7, 0x7)` 把 usbphy0/1+ref24m 三路参考钟全关 → 根口
   `PORTSC=0x00001000`（仅 PP，无 CCS）无声失败。正确写法 = 老代码的
   `(0x7 << 16)`（仅写使能、数据 0）。
2. **PMU `BUS_IDLE_SFTCON0` 只置写使能半字**：数据位一起置位会把 PIPE 总线岛
   压入 idle 请求 → 同步异常（`[port] FAULT`，FAR 在 PIPE 域访问）。老代码语义 =
   释放 idle 请求后等 ACK 清零。
3. **VBUS = GPIO3_A0/A1** 拉高（不拉高完全没电，KI-004）；once-guard 防二次 softrst。
4. **缓存契约**：`board_dcache_invalidate` 本就是 ivac-only，与 CherryUSB 契约一致；
   `CONFIG_USB_DCACHE_ENABLE` + `CONFIG_USB_EHCI_DESC_DCACHE_ENABLE` 必开
   （本仓无 .noncacheable 段）。
5. **枚举上限**：`REQUEST_BUFFER_LEN 512→2048`（AIC 配置描述符 1433 B，vendored
   G11 溢出检查会拒绝并枚举失败——检查本身工作正常）；`MAX_INTF_ALTSETTINGS
   8→16`（同设备 >8 altsetting 解析溢出）。
6. **`usbh_find_hubport` 的 assert 对 ext-hub 后设备必然停机**（要求
   `hub_index <= roothub->index=1`）；适配层改用自写安全树查找，查不到返回 NULL。
7. 本轮 `usb_hc_low_level_init` 中断安装走 CMSIS `IRQ_SetHandler/IRQ_SetPriority
   (API_CALL_RAW)/IRQ_Enable`，SPI 由 gicv3.c 初始化统一 IROUTER→core0，与 GMAC 同路。

## 基线问题（非本轮引入，记录待闭环）

见 `docs/evidence/20260919-issue-smp-shell-console-rx.md`。要点：4 核主镜像在
NET READY 后 shell 无响应（stash 对照 @`24a69b6` 实证与 M6-A 无关）；单核下
控制台 RX 会被 uart 驱动"line assert 风暴"启发式在启动早期禁用（当日 ≥6 次
启动全部复现，含基线镜像）。因此本轮镜像 SMP_CORES=1 且验收以启动日志为据
（USB 事件为 boot 期自动打印，不依赖 shell 交互）。

## 范围与遗留

- 本轮 = M6 枚举段：hub + 设备枚举；**未**写任何网卡/摄像头/MSC 类驱动（口径如此）。
- M6 收口遗留：`class/{msc,hid}` vendored + U 盘热插/MSC 读写判据 2/3、`usbh list -t`
  交互命令验证（待 console RX 问题闭环）、4 核镜像回归。
- iso 死路（G3/G4）不变；OHCI/TT 不做（hub 后 FS/LS 不支持，本轮设备全 HS 不受影响）。

## 收尾

- 板卡 `power_off{confirm:true}` 后 `power_status` 复核 power_on=false（2026-09-19 13:0x）。
