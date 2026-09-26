# freewebcamera

RK3568（E4AP5G1-ITX）上的 RTOS 综合工程：USB 摄像头 + WiFi + 网络工具的载体，
用真实应用横向验证板级外设。当前形态 = **Eclipse ThreadX 6.5.1（Cortex-A55 UP）**
+ CMSIS-RTOS2 抽象 + cherrysh 控制台 + gdb stub + NetBSD net80211/USB 主机栈无线线
（EHCI + xHCI 双主机）+ lwIP + wpa_supplicant + netutils 工具族 + FatFs 存储线。

BSD-2（自有代码）；第三方依赖全部为宽松许可（MIT / Apache-2.0 / BSD / ISC），见
[IMPORT-INFO.md](IMPORT-INFO.md)。

## 目录结构

```
app/                  应用入口与锚点（SHELL READY、诊断场景）
drivers/              设备驱动（UART、DW PCIe 主机 + MSI-X 端点编程）
include/              接口头（CMSIS 标准 + 自补缺口头：pcie.h/msi.h 等）——app/drivers 只准 include 这层
port/
  aarch64/            CPU bring-up：startup/mmu/gicv3(+ITS/MSI)/tick/cache/gdb stub
  board/              板级链接脚本与寄存器真值
  adapters/           组件适配层（每个第三方组件一个目录，见下）
third-party/          固定 pin 的 git submodule + vendored 目录（IMPORT-INFO.md 登记）
patches/              对 submodule 上游的全部本地差异（真值源；经 make sync 物化）
tools/                宿主侧工具（gdbinit.uc 等）
build/                构建产物（不提交）
```

分层是**构建系统强制的依赖方向**（`make gates` 的 check-deps 检查）：

```
app/ · drivers/  ──>  include/            只 include 接口头
hal/ · port/adapters/  ──>  third-party/  只有 adapter 能碰上游头
third-party/  ──× 反向依赖本项目任何东西
```

OS 抽象 = CMSIS-RTOS2（`cmsis_os2.h`）；驱动抽象 = CMSIS-Driver（`ARM_DRIVER_*`）；
中断 = CMSIS `irq_ctrl.h`（自研 GICv3 后端，含 ITS/MSI）。内核头（`tx_api.h` 等）只许
出现在 `port/adapters/`。**引第三方组件必带 `port/adapters/<组件>/` 适配层，否则视为未完成。**

## 第三方依赖（submodule + patches）

| 组件 | 形态 | 用途 |
|---|---|---|
| Eclipse ThreadX `v6.5.1` | submodule | UP 内核（cortex_a55 port） |
| CherrySH / CherryRB | submodule | 控制台 shell + 输入环 |
| NetBSD net80211 + USB 栈（netbsd-11 pin） | submodule（sparse） | 无线 + USB 主机：net80211、usbdi/uhub/**ehci/xhci**、if_urtwn + 固件 |
| lwIP `STABLE-2_2_1` | submodule | TCP/IP（sockets 供 iperf3/netutils） |
| wpa_supplicant-rtos | submodule | WPA2-PSK |
| TLSF / mpaland printf / RT-Thread netutils / iperf3_embedded / CMSIS 头 | vendored | 堆 / 格式化 / 网络工具族 / CMSIS 接口头 |

纪律：**submodule 工作树保持与 pin 逐字一致**；一切本地差异进 `patches/<组件>/`，经
`make modules` 重放、`make sync` 在 submodule 内物化为 `fwc/<组件>` 分支提交（其父提交即
pin，**绝不 push 到 submodule 的 origin**），gitlink 随父仓提交。依赖变更与 IMPORT-INFO.md
登记同一提交完成。

## 构建

```sh
make all      # build/rk3568-threadx-uc/threadx-uc.bin（默认目标）
make deploy   # 拷贝到 TFTP 根 /mnt/d/tftpboot/rtos.bin，前后 sha256 对拍
make modules  # submodule init + sparse-checkout + 重放 patches/
make sync     # 把 patches 物化为 submodule 内 fwc/<组件> 提交（改 patches / 收尾前必跑）
make gates    # cleanroom-scan（厂商痕迹零容忍）+ check-deps（分层依赖方向）——提交前必跑
make clean
```

工具链：xpack `aarch64-none-elf-gcc 13.2.1`（`AARCH64_CROSS_PATH` 可覆盖）。
`-mgeneral-regs-only -DGUEST`（EL1），镜像平铺 raw bin，加载地址 = 链接地址 = `0xa000000`。
改 Makefile 旗标或 force-include 头后需 `rm -rf build/rk3568-threadx-uc` 再构建（.d 不追踪）。

## 部署与运行

1. `make deploy` 把镜像拷到 TFTP 根（默认 `/mnt/d/tftpboot/rtos.bin`）；
2. 板子从 U-Boot 用 TFTP 载入到 `0xa000000` 后启动（本板用 U-Boot 的 `go` 命令；
   加载地址 = 链接地址）；
3. 串口 115200 进入 cherrysh；`version`/`uptime` 自检，`wlan start` 起无线，`net` 看 IP。

板卡实验室的具体操作（电源/串口/TFTP 服务、冷启动要求、gdb 桥、首条命令预热等）记录在
工作区的 `../AGENTS.md` 与 `~/.agents/skills/rk3568-lab/`，属于开发环境说明，不随本仓发布。

### shell 命令速查（cherrysh）

```
wlan start | status | scan [s] | nic <iwm|urtwn|auto> | dump | hist [n] | delaytest
wlan nic iwm          # 只起 PCIe 线（跳过 USB）；urtwn 反之；auto = 双线（默认）
pcie init | dump      # DW PCIe 主机：固件继承的链路 + 端点/BAR/能力列表
heap                  # 系统堆：空闲字节 + ≥2KB 存活块（mbuf 一眼可辨）
wlan xhci | xreg | reg read|write|txq / chanmap / callouts / calib / usbdebug <n>
wpa connect <ssid> <psk> | status
net                   # 接口/IP/网关；ping / tftp / ntp / telnetd / iperf3 <host> [sec] [-r] [-u]
its | itsdump | uartint <n> | uptime | tick | gicdiag | version
```

## 贡献与流程

- trunk-based：`trunk` 是稳定合入点，功能走 `feat/xxx`、缺陷走 `fix/xxx`，完成后单独合入。
- 第三方组件一律 submodule + `patches/<组件>/`：`patches/` 是真值源，`make modules` 重放、
  `make sync` 在 submodule 内物化为 `fwc/<组件>` 提交（其父提交即 pin），**绝不 push 到
  submodule 的 origin**；依赖变更与 `IMPORT-INFO.md` 登记同一提交完成。
- 提交前跑 `make gates`（clean-room 厂商痕迹扫描 + 分层依赖方向检查）。
- 开发过程文档（issue/决策/验收证据）不随本仓发布，见工作区 `../issues/`、
  `../evidences/<run-id>/` 与 `../NOTICE.md`；硬约束（clean-room、接口宁少勿多、
  引组件必带 `port/adapters/<组件>/` 适配层）见工作区 `../AGENTS.md`。
- 补丁工作流与逐个补丁的理由：见 [patches/README.md](patches/README.md)。

## 当前能力基线（2026-09-26，全部今日实测）

- **载体**：ThreadX UP + gdb stub（raw TCP 串口桥 RW 18000）+ cherrysh，ITS/LPI 多次冷启全绿。
- **USB 主机**：EHCI1（fd880000 面板口，CH334P hub）+ **xHCI（fcc00000 上层 USB3 口，
  DWC3 otg 强制 host）**；域自举顺序硬约束（USB3 域 SRST 先于任何 HCD attach）。
- **无线（USB）**：rtl8188eus（urtwn）经 xHCI 全链路实测——枚举/WPA2/DHCP
  （租约 192.168.0.249）/ping 网关与宿主双向 0% 丢包（iperf 长测下一轮补）。
- **无线（PCIe）**：Intel AC7260（iwm）经原生 DW PCIe 主机驱动 + ITS/MSI 起来：
  链路继承、固件 17.352738.0、扫描、WPA2 关联与 DHCP 租约成立；**单播数据面当前 blocked**
  （广播/DHCP 通、ARP/ICMP 不通；PCIe 单线配置会复现固件致命错误），排查记录见
  `../issues/20260926-feat-net80211_refine.md`。
- **网络**：lwIP 2.2.1 + wpa_supplicant（WPA2-PSK）+ netutils（ping/tftp/telnet/ntp/
  tcpdump）+ iperf3_embedded。
