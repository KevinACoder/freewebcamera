# ThreadX UP GDB 调试载体验收

- 日期：2026-09-24
- 分支：`wip/threadx-uc-gdb`
- 载体：`make threadx-uc`，单核 ThreadX UP，`-Og -g3`
- 板卡：RK3568 E4AP5G1-ITX，经 oslab MCP 串口/TFTP

## 构建与部署

构建通过，最终镜像为 `966376` 字节：

```text
sha256  eda9ebdcc9ce0864b0de54c758c3e4fe7ef2f82ac3465f68fe73af7f0a58d4d5
crc32   80a04bc0
```

TFTP staging 返回同一 SHA-256；U-Boot 记录 `Bytes transferred = 966376 (ebee8 hex)`，
按板卡纪律以 `go 0xa000000` 启动。

启动日志关键行：

```text
gdb: boot EL=8 MDCR_EL2=6 DAIF=3c0 MDSCR=0
gdb: DBGAUTHSTATUS=ff OSLSR=a
gdb: OSLSR after unlock=8
gdb: dbgbvr0 readback=deadbeec (expect deadbeec)
gdb: hw breakpoints=6 watchpoints=1
```

`OSLSR` 从 `a` 变为 `8`，说明 OS lock 已被清除；EL1 对调试寄存器的写入保持有效。

## GDB 行为

使用 `tools/gdb/gdbinit.uc` 连接 `192.168.0.18:18000`，每项均从场景 6 的 gate BRK
停点开始，测试后 detach 并冷启动下一项。

| 能力 | 结果 | 证据 |
| --- | --- | --- |
| Z0 软件断点 | PASS | `break *dbg_probe_breakpoint` 停在 `0xa054138` 原地址；修复后不跳过被替换指令 |
| 寄存器写入 | PASS | `set $x0 = 0x12345678` 后立即读回相同值，再写回 0 |
| 单步 | PASS | `stepi` 使 PC `0xa054100 -> 0xa054104`；两次停止/继续 CPSR 均 `0x60000004` |
| Z1 硬件断点 | BLOCKED（硬件事件未投递） | `bcr=1e3 bvr=<目标> mdscr=a000` 可读回，但执行穿过目标后只在 gate BRK 停止 |
| Z2 写监视点 | BLOCKED（硬件事件未投递） | `wcr=1ff3 wvr=a501080 mdscr=a000` 可读回；`dbg_probe_word` 确实写成 `0x3568`，未收到 watchpoint trap |

硬件事件未投递不是寄存器写入失败：启动链为 BL31 + OP-TEE + U-Boot，入口为 EL1，
但 `MDCR_EL2=6` 未置 `TDE`；EL1 无法观察或修改更高异常级的调试路由。因此当前载体
把 `break` 默认走 Z0（`hardware-breakpoint-limit=0`），Z1/Z2-4 保留供更换启动链或
板卡后的对照实验。

## 代码修正

- 启动记录入口 EL、`MDCR_EL2`、`DBGAUTHSTATUS` 与 OS lock 解锁后的状态。
- 修正 Z0 命中处理：场景 gate 的内置 BRK 跳过自身，Z0 补丁断点停在原指令地址，
  由 GDB 负责越过并恢复补丁。
- 单步期间只屏蔽 A/I/F，清除 D 位，停止后恢复原 DAIF；避免单步遗留屏蔽位。
- RSP `P` 命令按寄存器宽度解析并初始化缓冲区，支持 CPSR 32 位和通用寄存器 64 位。

## 门禁

`make threadx-uc`、`make all`、`make freertos`、`make k4`、`make gates`、
clean-room 扫描、依赖检查及 `git diff --check` 均通过。

