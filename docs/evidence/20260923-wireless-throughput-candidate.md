# SDIO WiFi 吞吐候选：数据队列与 OFDM 速率

日期：2026-09-23。状态：候选已上板短测，未达到长测门槛。

基于 M11 r4 B2b 稳定基线（下行 1.10、上行 2.69 Mbit/s，均 600s），
`net_80211` `d2c7d9f..7e8175c` 将单播数据帧从管理队列/6 Mbps
迁到 BE/低队列，速率可用 `wlan rate 24|36|54` 调整，默认 24 Mbps。
BE→低队列与芯片初始化中的 TRXDMA 映射一致，FREE_TXPG 轮询随之读低队列页数。
管理帧与组播保持原队列和速率。`wlan select rtw8189f` 在 `wpa start`
前锁定诊断目标，`wlan status` 显示当前卡及 MAC，`wlan reg` 随所选卡分发。

本轮 `make all` 通过；锁契约修复后 ThreadX 镜像 sha256：
`c57dd1cc9244fa1cb0240a0ba256e2b43c8cbc5e385feb28ddf8ca27d93adbf5`。
`make freertos`、`make ktest`、`make gates`（含 K4、clean-room、依赖方向）均通过。
后续修复 CMSIS `osKernelLock` 返回值契约并重建，板上 24、36、54 Mbps
上行 30s 分别实测 5.46、5.48、5.58 Mbit/s，均完整结束且无 fatal；
反向下行 30s 约 0.66 Mbit/s，未达到目标，故未启动 600s 验收。
无线 `decryptcrc=0`，lwIP bridge `pbuf_fail=0/take_fail=0/input_fail=0`。
尚需逐级测试 24、36、54 Mbps
的关联/重传/双向 30s，再以最佳稳定档做两次冷启动、双向 600s、
ping 20/20、INPKT/HEAP/无线错误计数对拍。目标为上行至少 9.7、
下行至少 9.0 Mbit/s；若仍低，再根据服务器重传簇验证 RX 聚合候选。

板卡工具仅允许 oslab MCP，本会话未暴露该工具，故未 staging、未启动
Windows iperf3 服务、未做今日实测。已授权的服务端路径为
`D:\\Software\\iperf3.19.1`。
