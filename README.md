# freewebcamera

RK3568（E4AP5G1-ITX）上的 RTOS 综合工程：以"USB 摄像头 + 无线网卡 + 网络应用"
这一真实组合为载体，验证板级 PCIe / USB / 无线外设在硬实时内核上的完整数据面。
当前形态 = **Eclipse ThreadX 6.5.1（Cortex-A55 UP）** + CMSIS-RTOS2 抽象 +
cherrysh 控制台 + gdb stub + NetBSD libbsd（net80211/USB 主机栈，EHCI + xHCI 双主机）+
lwIP + wpa_supplicant + netutils 工具族。

BSD-2（自有代码）；第三方依赖全部为宽松许可（MIT / Apache-2.0 / BSD / ISC），
见 [IMPORT-INFO.md](IMPORT-INFO.md)。

## 支持硬件

| 类别 | 硬件 | 驱动 |
|---|---|---|
| SoC | RK3568（Cortex-A55，单核运行） | 移植自上游 + 板级适配 |
| 无线（PCIe） | Intel AC7260（M.2/x4 槽） | NetBSD `iwm` + 自研 DW PCIe 主机 + ITS/MSI-X |
| 无线（USB） | Realtek RTL8188EU | NetBSD `urtwn` + EHCI/xHCI 主机 |
| 摄像头（规划） | UVC USB 摄像头 | NetBSD `uvideo` |
| 控制台 | UART2（115200） | NS16550 兼容 |

## 实现方法

- **OS 抽象** = CMSIS-RTOS2（`cmsis_os2.h`），内核以 ThreadX 落地；换内核不动组件。
- **驱动抽象** = CMSIS-Driver 风格接口头（`include/`，含自补的 `pcie.h`/`pci.h`/
  `msi.h`）；SoC 集成与寄存器真值收口在 `port/board/`，IP 驱动（`drivers/`）不携带
  板级坐标。
- **第三方组件** = 固定 pin 的 submodule + `patches/<组件>/` 本地差异（真值源），
  经 `make modules` 重放、`make sync` 物化；依赖与出处登记在 `IMPORT-INFO.md`。
- **分层**是构建系统强制的依赖方向（`make gates` 检查）：`app/ · drivers/` 只 include
  `include/`；只有 `port/adapters/<组件>/` 适配层能触碰上游头；`third-party/` 禁止
  反向依赖本项目。

## 目录结构

```
app/                  应用入口与锚点（SHELL READY、诊断场景）
drivers/              平台无关 IP 驱动（UART、DW PCIe 主机 + MSI-X 端点编程）
include/              接口头（CMSIS 标准 + 自补缺口：pcie.h/pci.h/msi.h 等）
port/
  aarch64/            CPU bring-up：startup/mmu/gicv3(+ITS/MSI)/tick/cache/gdb stub
  board/              板级链接脚本、平台坐标与寄存器真值
  adapters/           组件适配层（ThreadX/CMSIS 桥、libbsd（NetBSD 面）、lwIP、wpa_supplicant、cherrysh）
third-party/          固定 pin 的 git submodule + vendored 目录
patches/              对 submodule 上游的全部本地差异（真值源；经 make sync 物化）
tools/                宿主侧工具（gdbinit.uc 等）
```

## 构建与部署

```sh
make all      # build/rk3568-threadx-uc/threadx-uc.bin（默认目标）
make deploy   # 拷贝到 TFTP 根 /mnt/d/tftpboot/rtos.bin，前后 sha256 对拍
make modules  # submodule init + sparse-checkout + 重放 patches/
make sync     # patches 物化为 submodule 内 fwc/<组件> 提交（改 patches / 收尾前必跑）
make gates    # clean-room 厂商痕迹扫描 + 分层依赖方向检查（提交前必跑）
make clean
```

工具链：xpack `aarch64-none-elf-gcc 13.2.1`（`AARCH64_CROSS_PATH` 可覆盖）。
`WLAN_NIC=iwm|urtwn|all` 编译期选择无线线（`make clean` 后切换）。镜像为平铺 raw
bin，加载地址 = 链接地址 = `0xa000000`；板上经 U-Boot `tftp` 至 `0xa000000` 后 `go`
启动，串口 115200 进入 cherrysh。

## 测试方法

板卡实验室操作（电源/串口/TFTP 服务、冷启动要求、gdb 桥）记录在工作区
`../AGENTS.md` 与 `~/.agents/skills/rk3568-lab/`，不随本仓发布。镜像起来后的
功能验证梯（cherrysh）：

```sh
wlan start                       # 起无线（PCIe iwm / USB urtwn）
wlan scan                        # 列出可见 AP
wpa connect <ssid> <psk>         # WPA2-PSK 关联
net                              # DHCP 拿 IP；显示接口/网关
ping <网关或宿主 IP>              # 双向连通性
iperf3 <宿主 IP> 600             # 吞吐长测（push）；-r 反向，-u UDP
wlan status                      # 驱动计数器/状态诊断；pcie dump 看主机与端点
heap                             # 系统堆水位
```

## 功能与已验证状态（2026-09-27 实测）

- **无线（PCIe，Intel AC7260）**：DW PCIe 主机（链路继承 + iATU + ITS/MSI-X）→
  扫描 → WPA2-PSK（CCMP）→ DHCP → ping 双向 0% 丢包；**iperf3 600 s 双向长测
  通过**（无 panic、无固件错误）；冷启动 ×3 行为一致。
- **无线（USB，RTL8188EU）**：xHCI 链路枚举/WPA2/DHCP/ping 双向 0% 丢包实测通过。
- **网络**：lwIP 2.2.1 + wpa_supplicant（WPA2-PSK）+ netutils（ping/tftp/telnet/
  ntp/tcpdump）+ iperf3_embedded。
- **USB 主机**：EHCI + xHCI 双主机；USB3 域自举顺序与 USB2 面板口已验证。
- **已知边界**：iwm 吞吐受当前单帧发送路径限制（无聚合），与 USB 网卡量级有差距；
  吞吐优化单列专项。

## 许可

自有代码 BSD-2；第三方组件许可见 `IMPORT-INFO.md` 与各自上游声明。开发过程文档
（issue/决策/验收证据）不随本仓发布，见工作区 `../issues/`、`../evidences/` 与
`../NOTICE.md`。
