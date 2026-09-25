# freewebcamera

RK3568（E4AP5G1-ITX）上的 RTOS 综合工程：USB 摄像头 + WiFi + 网络工具的载体，
用真实应用横向验证板级外设。当前形态 = **Eclipse ThreadX 6.5.1（Cortex-A55 UP）**
+ CMSIS-RTOS2 抽象 + cherrysh 控制台 + gdb stub + NetBSD net80211/USB 主机栈无线线
（EHCI + xHCI 双主机）+ lwIP + wpa_supplicant + netutils 工具族 + FatFs 存储线。

BSD-2（自有代码）；第三方依赖全部为宽松许可（MIT / Apache-2.0 / BSD / ISC），见
[IMPORT-INFO.md](IMPORT-INFO.md)。

## 目录结构

```
app/                  应用入口与锚点（M0 锚点、SHELL READY、诊断场景）
drivers/              设备驱动（UART、GMAC、AHCI/DWC、NVMe、DWMHC …）
hal/                  硬件抽象（板级常量与控制器封装）
include/              接口头（CMSIS 标准 + 自补缺口头）——app/drivers 只准 include 这层
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
make k4       # 内核接缝验证：app/drivers 对 CMSIS stub 链接成功
make clean
```

工具链：xpack `aarch64-none-elf-gcc 13.2.1`（`AARCH64_CROSS_PATH` 可覆盖）。
`-mgeneral-regs-only -DGUEST`（EL1），镜像平铺 raw bin，加载地址 = 链接地址 = `0xa000000`。
改 Makefile 旗标或 force-include 头后需 `rm -rf build/rk3568-threadx-uc` 再构建（.d 不追踪）。

## 上板

全部板卡访问走 oslab MCP（`mcp__oslab__*`，见 `~/.agents/skills/rk3568-lab/`）：

1. `service_status` 全绿 → `board_acquire`；
2. `make deploy` → `boot_os {os:"rtos"}`（自动 KI-001 冷断电 ≥6s + tftp 0xa000000 + `go`；
   **本板 bootelf 必崩，只用 `go`**）；
3. 串口交互：一行一令、`\r` 结尾、发完等 ≥1.2s；**首次开机第一条 shell 命令会被吞**（先发
   一条废命令预热）；`serial_read` 按行组装，无换行输出不可见；
4. 收尾：`board_release` → `power_off{confirm:true}` → `power_status` 复核。

### shell 命令速查（cherrysh）

```
wlan start | status | scan [s] | dump | hist [n] | delaytest
wlan xhci          # xHCI 状态 + 命令/事件环内存窗（判「命令超时」的第一现场）
wlan xreg          # xHCI capability/operational/runtime/doorbell 原始寄存器行
wlan reg read|write|txq / chanmap / callouts / calib / usbdebug <n>
wpa connect <ssid> <psk> | status
net                # 接口/IP/网关；ping / tftp / ntp / telnetd / iperf3 <host> [sec] [-r] [-u]
stor ls|mount|...  # FatFs 卷（sata0/1、nvme0、emmc0）
its | itsdump | uartint <n> | uptime | tick | version
```

## 开发流程

- **trunk-based**：`trunk` 是稳定合入点；功能 = `feat/xxx`，缺陷 = `fix/xxx`，完成后
  `merge --no-ff` 单独合入。并行时 `git worktree add <wt> feat/xxx`（板卡物理互斥由
  oslab `board_acquire` 保证）。
- **每轮收尾 git 状态必须干净**：patches 物化、gitlink 提交、无未跟踪文件。
- 过程文档（issue/决策/验收证据）**不入本仓**：决策与 run 记录在工作区
  `../issues/`、`../evidences/<run-id>/`；路线图在工作区 `../NOTICE.md`。
- 硬约束继承（clean-room、无厂商痕迹、接口宁少勿多）见工作区 `../AGENTS.md`。

## 当前能力基线（2026-09-25，全部今日实测）

- **载体**：ThreadX UP + gdb stub（raw TCP 串口桥 RW 18000）+ cherrysh，ITS/LPI 五次冷启全绿。
- **USB 主机**：EHCI1（fd880000 面板口，CH334P hub）+ **xHCI（fcc00000 上层 USB3 口，
  DWC3 otg 强制 host）**；域自举顺序硬约束（USB3 域 SRST 先于任何 HCD attach）。
- **无线**：rtl8188eus（urtwn）经 xHCI 调通——枚举/WPA2/DHCP/**iperf3 600s 上行 10.39 /
  下行 9.20 Mbit/s（0 重传）**；EHCI 时代基线 9.96 / 8.08。rtl8189ftv（SDIO）枚举通过。
- **网络**：lwIP 2.2.1 + wpa_supplicant（WPA2-PSK）+ netutils（ping/tftp/telnet/ntp/
  tcpdump/netio）+ iperf3_embedded。
- **存储**：SATA×2 + NVMe + eMMC（24MHz 写）经 FatFs 打通。
