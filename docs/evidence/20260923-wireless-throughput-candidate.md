# USB WiFi 吞吐候选：适配器诊断与传输分段计时

日期：2026-09-23。状态：构建通过，板上性能未验证。

`net_80211` `7e8175c` 的 usbdi shim 为每笔成功传输记录三段
1 ms 粒度的聚合时间：提交到 HCD 武装（queue）、武装到 EHCI 回调
（hcd）、EHCI 回调到 worker 取件（wake）；`wlan usbstats` 显示
TX/RX 各段样本数、累计与最大值。计数不打印逐帧日志。
`wlan select urtwn` 在 `wpa start` 前指定诊断目标，`wlan status`
显示卡及 MAC，`wlan reg` 按所选卡访问，避免双卡并存时误读 SDIO。
还补齐 `wlan_lwip_get_netif` 声明并在初始化后再取 netif。

本轮 `make all` 通过；ThreadX 镜像 sha256：
`d12091002231f2810c9d03dbf67904c7a7506097c0f3620d9ac81e3c1577147f`。
`make freertos`、`make ktest`、`make gates`（含 K4、clean-room、依赖方向）均通过。
该哈希只证明候选可构建，未部署到板卡。现有 R-A 板测的 30s
稳定值为 2.34-2.36 Mbit/s，早期短时曾达 10.51 Mbit/s，
均不能代替本候选的持续结果。下一次板测先读两次 `wlan usbstats`
差值与吞吐区间，对照 queue/hcd/wake 的每笔均值与最大值定位 2M 限速；
仅按观测结果选择 USB 修复，再做双向 600s、两次冷启动的验收。
目标为双向至少 11 Mbit/s。

板卡工具仅允许 oslab MCP，本会话未暴露该工具，故未 staging、未启动
Windows iperf3 服务、未做今日实测。已授权的服务端路径为
`D:\\Software\\iperf3.19.1`。
