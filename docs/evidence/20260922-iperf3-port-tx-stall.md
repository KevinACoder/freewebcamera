# M7+ 证据：iperf3_embedded 移植板验（协议互通已证实 / wlan TX bulk 停摆新 bug）

日期：2026-09-22　提交：freewebcamera `ac1716f`（wip/m7-net80211）
镜像：`make all` ThreadX SMP 主线，`build/rk3568-threadx/threadx-smp.bin`，
部署 sha256 `a68383ce…`，U-Boot tftp 949992 字节 + crc32 对拍 `d2cff02d` 一致。
对端：Windows 宿主 192.168.0.18（TFTP serverip 同机）上**已在运行**的 iperf3 3.19.1 server（5201）。

## 1. iperf3 移植验证（PASS 的部分）

冷启动 → urtwn 附着零 fault → `wpa start`（if_init flags=8843）→
`wpa connect QiQiJia paopaojie520`：

```
wlan0: Associated with 14:a3:2f:18:07:2c
wlan0: WPA: Key negotiation completed ... [PTK=CCMP GTK=CCMP]
wlan0: CTRL-EVENT-CONNECTED - Connection to 14:a3:2f:18:07:2c completed
wl: ip=192.168.0.249 gw=192.168.0.1 mask=255.255.255.0
wl: link=up dhcp_bound=yes
```

`net down 0`（gmac0 让出路由）→ `ping 192.168.0.18`：2/2 回包 2-4 ms（wlan 路径）。

`iperf3 192.168.0.18 10`——**与真实 esnet iperf 3.19.1 协议互通成功**
（AI 重实现的上游风险被证伪）：

```
[iperf3] info:  control connected
[iperf3] info:  sending cookie: iperf3-embedded.329323.8792 (37 bytes padded)
[iperf3] info:  sending params: {"tcp":true,"omit":0,"time":10,...,
                                 "client_version":"3.19.1"}
[iperf3] info:  server ack 1 byte(s): 0a 00 00 00 00 00   ← TEST_START
[iperf3] info:  TCP forward start, 10 seconds
   1-   2 sec       0.09 Mbits/sec                        ← 第一个 interval
```

LWIP_SOCKET=1 + SO_RCVTIMEO 路径、iperf3_port CMSIS 映射（线程创建/
CNTVCT 计时/诊断输出）全部实测工作。

## 2. 新 bug：wlan TX bulk 停摆（OPEN）

现象：第一个 interval 仅 0.09 Mbits/sec 后，测试无后续输出（iperf3 工作线程
阻塞在 `send()`——lwIP snd_buf 满后 netconn write 无限期等待）。

现场固定：

- `wlan stats`：**tx=57 冻结**（隔 8s 两次读数不变），txerr=0；
  rx 同期 3698→4306→…（beacon 不断），state=RUN，ch=2412；
- unicast txpn=46：密钥安装后只发过 ~46 帧单播（≈管理帧+ping+前 11KB 数据）；
- 数据 TX 约 10 个 bulk 帧后死亡：后续 `ping 192.168.0.1 2` 2 发 **0 回**
  （burst 前同命令 4/4 通）——TX 管道整体死了，不止 bulk 数据流；
- 测试开始时刻伴随两条 `irq: spurious`（332.800/332.805）；
- shell / 其余子系统全程存活，无 fault、无 BAD FREE、无 lwip 断言。

定位面（下一步，未做）：urtwn TX 完成回收路径（2 tx pipes 的 txdesc 环）
或 usbdi shim 的 TX URB 完成回调（ISR 锁存 + worker 补完形态是否覆盖
bulk TX）；RX 正常说明 EHCI 中断/完成 worker 活着，嫌疑集中在 TX 侧
URB 未完成或 txdesc 不回收。verbatim 的 `if_urtwn.c` 不改，修复落点在
usbdi shim / urtwn_reg 适配层。

## 3. 遗留与备注

- `gmac1 mac initialize failed` 本次冷启动出现一次（已知偶发，与 wlan 无关）；
- 命令面补充项：`iperf3` 命令缺 `stop` 子命令（库有 `iperf3_client_stop`，
  但 TX 阻塞时 stop 也等不到 worker 退出，需强制回收语义）；
- 输出在 shell 交互时会与 csh_printf 并发交错（两个非同锁输出口），
  不影响功能，诊断期可接受；
- **部署通道备注**：`os=standalone` profile 加载的是 `baremetal.bin`
  （2026-09-16 的旧产物，317168B）。主线镜像部署名是 `freertos.bin`
  （make deploy），本次走 U-Boot 手动 `tftp freertos.bin` + crc32 对拍 +
  `go 0xa000000` 完成；后续应使用 `boot_os os=freertos` profile 或把
  standalone 映射改指 freertos.bin，避免再次拉起 stale 镜像。
- Windows 侧 iperf3 server：`/mnt/d/Software/iperf3.19.1/iperf3.exe -s`
  已在宿主常驻（另启动会报 Address already in use）；3.1.4 版本也在。

## 构建门禁（本次全绿）

`make all`（ThreadX SMP，5981520B elf）、`make freertos`（单核对照，
5546280B）、`make ktest`、`make gates`（cleanroom-scan PASS + check-deps PASS）。
