# cherryusb 基线升级 0e40349b → dev 5019680e & HCD persistent-QH 实验撤销

日期：2026-09-23　分支：`wip/urtwn-usb`（fwc-urtwn worktree）　状态：已实施，板测回归待跑

## 结论

1. **vendored cherryusb 升级到 cherry-embedded dev `5019680e`**（用户向社区反馈后
   上游更新）。与本线相关的两条：
   - `6c40a28` fix(port): safely check urb->hcpriv validation in usbh_kill_urb ——
     上游落地了 `qh->killed` 握手（1=killed 等待 IAA 池扫描 ack，2=已 ack，
     killer 上下文负责 free+complete）与 `!urb->hcpriv` 早退 `-USB_ERR_INVAL`。
     **本地追加 1① 的 kill 握手实现（含 `usbh_kill_urb_wait_advance` 内联回退）
     删除，整体采用上游版**；shim 侧 watchdog/abort 路径的 `hcpriv` 守卫由上游承担。
   - `5019680e` feat: add register driver api —— 官方多控制器注册 API
     （`usbh_hc_driver` / `usbh_register_hc_driver`，`CONFIG_USBHOST_MULT_HC` 门控）。
     与本仓 D50 原型（`usbh_hcd_ops` / `CONFIG_USBHOST_MULTI_HCD`）**机制同构、
     宏名不同、互不激活，两套并存**；待后续评估迁移到官方 API 后撤 D50 原型。
2. **升级后 vendored 残余差量收敛为四项**（对上游 dev 逐文件 diff 复核）：
   D50 ops 分发、bulk OUT toggle 软件化（追加 2）、C3 `USBH_URB_ZERO_PACKET`
   零长 qTD、`osal/usb_osal_freertos.c` sem max=0xFFFF。差量重导出：
   `web_camera/cherryusb-local-vs-upstream-5019680e.patch`
   （361 行，sha256 `c8c761abfcfa67824f8c1b47b806b638ab30208a02ee9216b2ed27044c6ffc6a`；
   旧版 0e40349b patch 为 489 行——kill 握手 hunks 已归还上游）。
3. **撤销 codex 未提交的 HCD persistent-QH 实验**（`usb_hc_ehci.c/.h` +
   `usbdi_compat.c` 三文件，4d5907a 之上的工作树改动）。撤销依据：
   - 与 CherryUSB 社区契约相悖："一 endpoint 同时只允许一笔 URB 在途"，
     多笔由调用方挂 `usbh_urb.list` 排队、完成后续投；队列不进 HCD。
   - 板测楔死（`20260923-wireless-throughput-candidate.md`：首个 1s 区间
     12.74 Mbit/s 后完成停摆）。楔死机理与实验代码的缺口一致：提升下一笔
     queued URB 时**无 QH halt 状态检查、无 IAAD doorbell、队列空即 free QH**
     ——缺 NetBSD `ehci_set_qh_qtd`/`ehci_del_qh` 的摘链握手与 doorbell 语义。
   - 实验补丁存档：`web_camera/fwc-urtwn-hcd-persistent-qh-experiment-4d5907a.patch`
     （379 行，sha256 `7acd516eb0f6d356ddc455f4f71e6bef08e45e76c4334e3d6159980a89d5cd87`）。
   - 值得保留的观测：楔死前 1s 冲到 12.74 Mbit/s ≈ NetBSD N6 包络（12.2M），
     说明 USB/88E 硬件路径不是 2.3M 的天花板——瓶颈在每帧节奏（~6ms/帧）。

## 升级对 urtwn 线的行为影响

- `usbh_kill_urb` 语义变化：上游版超时（200000 次自旋）返回 `-USB_ERR_TIMEOUT`
  且不再补完成（旧本地版有内联回退收尾）。shim watchdog 对 bulk 的 kill 在
  IAA 正常到达时路径不变；IAA 丢失场景从"本地内联收尾"变为"返回超时"——
  该场景在 D40/D41 风暴归因后已属异常路径，回归板测覆盖。
- `usbh_submit_urb` 对同一 endpoint 多笔并发依旧不排队（`usbh_urb.list`
  预留字段上游仍无人使用），shim per-pipe xq 队列（de406f2）承担排队——契约不变。

## 后续（本仓 Phase 1–3）

板测回归（枚举 + 30s up 无回退）→ usbstats 三段计时归因 6ms/帧 → 契约内
节奏修复 → iperf3 -R/-u 阶梯对标 NetBSD N6（TCP 上 12.2 / 下 12.1 Mbit/s）。
