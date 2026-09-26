# IMPORT-INFO.md — third-party provenance

Trunk-baseline dependency policy (NOTICE.md): every component with a real
upstream git home is a **pinned git submodule**; any local deviation lives in
`patches/<component>/` (today: the five `net80211` patches and the one
`wpa_supplicant` patch — see `patches/README.md`). Components
without a usable upstream git home are vendored, with provenance in-tree.

A component without an adapter is unfinished: `third-party/threadx` ↔
`port/adapters/threadx/`, `third-party/cherrysh` ↔ `port/adapters/cherrysh/`;
`third-party/cherryrb` is adapter-internal glue (the console RX ring).

## Registered components

| Component | Upstream | Pin | License | Form | Used for |
|---|---|---|---|---|---|
| Eclipse ThreadX | `github.com/eclipse-threadx/threadx` | `44d7c95c582d415c4ad84527180b29c93c3bf664` (`v6.5.1.202602a_rel-1`) | MIT | submodule `third-party/threadx` | UP kernel: `common/src` (185 TUs) + `ports/cortex_a55/gnu`. Compiled set = the Makefile `KERNEL_SRCS` wildcard. Byte-identical to the pin (diff-verified against the frozen clone). Not compiled: every other architecture port, the SMP source set, trace (switched off in the adapter's `tx_user.h`). |
| CherrySH | `github.com/cherry-embedded/CherrySH` | `8efe539c6e55b71d2f2cd1116cd00c4d64222a67` (`v1.0.1-20`) | Apache-2.0 | submodule `third-party/cherrysh` | Console shell + the cherryrl readline. Compiled set: `chry_shell.c`, `builtin/{help,clear,shsize}.c`, `cherryrl/chry_readline.c`. `csh_config.h` is the adapter's shadow config (upstream's `csh_config_template.h` must not shadow it). Not compiled: `builtin/{login,lsusb}.c`, `samples/`, `doc/` — `lsusb` returns with the USB feat line. |
| CherryRB | `github.com/cherry-embedded/CherryRB` | `19ea7c6efcf19dc9e805a0a662212533ee5b1edb` (`v1.0.0`) | Apache-2.0 | submodule `third-party/cherryrb` | Single-producer/single-consumer ring for console input. No barriers upstream by design — the producer/consumer ordering contract lives in `port/adapters/cherrysh/cherrysh_adapter.c`. |
| TLSF | tlsf.baisoku.org, v3.1 package (2016-04-10) | v3.1 | BSD-2 (header notice) | vendored `third-party/tlsf/` (2 files + `PROVENANCE.md`) | System heap (`port/adapters/threadx/heap.c` binds it to the kernel allocation hooks). No canonical upstream git home: the vendored bytes match the official baisoku.org 3.1 package and differ from every commit of the mattconte GitHub mirror, so it is vendored rather than submoduled. Its bare `printf` error paths are served by `port/aarch64/minilibc.c` (routed through the locked console sink). |
| CMSIS interface headers | `github.com/ARM-software/CMSIS_6` @ `26206e47dcf0abfbdc64eb753a0b6334b24439f6` | v6.x | Apache-2.0 | vendored subset in `include/` | `cmsis_os2.h`, `os_tick.h`, `irq_ctrl.h`, `Driver_Common.h`, `Driver_USART.h` + `LICENSE.cmsis`. The GIC backend behind `irq_ctrl.h` is our own GICv3 (CMSIS ships only a GICv2 one). |
| NetBSD net80211 + USB host stack | `github.com/KevinACoder/netbsd-src` (branch `netbsd-11`; upstream of upstream: `github.com/NetBSD/src`) | `111525e242df83d65ff69e7bcbde03e141ce3e08` | BSD-2/ISC/BSD-3 family (per-file, NetBSD Foundation & contributors) | submodule `third-party/net80211` (recorded URL = local path `/home/zhugy/workspace/rk3568_lab/os/netbsd/src`; local-path submodule requires `git config protocol.file.allow always` — the `modules` target passes it via `-c`) | The wireless + USB host feat line: compiled subset is `sys/net80211` (28-file set, minus acl/tkip/wep/ioctl/rssadapt, per the frozen imports.md line), `sys/dev/usb` core (usbdi/usbdi_util/usb_mem/usb_subr/usb/usb_quirks/usbdevs/uhub/ehci) and `sys/dev/usb/if_urtwn.c`. The submodule's working tree is kept sparse to those subtrees + `external/realtek/urtwn` (firmware dist). All kernel shims live in `port/adapters/net80211/` — the pin stays byte-identical to upstream; deviations go through `patches/net80211/` (five: sysctl compile-out, aes-ccm
mbuf order, usbdi miss-out-ack log, urtwn cmd-ring overflow keeps the oldest
command, xhci debug level from `XHCI_DEBUG_DEFAULT`) and are materialized inside the submodule as branch `fwc/net80211` via `make sync` (the recorded gitlink points at that fwc commit; its parent is the pin; the fwc commit is never pushed to the submodule's origin). The USB debug/history world is wired in by force-including the compat `opt_usb.h` (`USB_DEBUG`/`USB_DEBUG_DEFAULT`/`EHCI_DEBUG`, history ring 4096 records — the upstream 50000 is ~3 MB of .bss). The PCIe/AC7260 line (feat/pcie-ac7260) extends the compiled subset with `sys/dev/pci/if_iwm.c` (the netbsd-11 iwm driver, compiled verbatim inside `port/adapters/net80211/iwm_reg.c`; headers `sys/dev/pci/{pcireg.h,pcivar.h,pcidevs.h}` and `sys/arch/arm/include/pci_machdep.h` join the sparse set — pin unchanged, no new patches) and is served by the native DW PCIe host driver `drivers/dwc_pcie.c`/`dwc_msix.c` (first-party, embox-provenance line) behind the frozen `include/pcie.h` interface, with the pci(9)/workqueue(9) glue in `port/adapters/net80211/pcie_glue.c`. The 7260 firmware blob `iwlwifi-7260-17.ucode` is embedded as `port/adapters/net80211/fw_iwlwifi7260.c` (Intel redistributable license beside the Realtek one in `port/adapters/net80211/firmware/`), served to iwm's firmload(9) calls through the existing `osal/firmware_cmsis.c` registry. |
| lwIP | `github.com/lwip-tcpip/lwip` | `77dcd25a72509eb83f72b033d219b1d40cd8eb95` (`STABLE-2_2_1_RELEASE`) | BSD-3 | submodule `third-party/lwip` | Network stack: `src/core`, `src/core/ipv4`, `src/core/dns.c`, `src/api` (tcpip/netifapi/err + the sequential API: sockets/api_lib/api_msg/netbuf), `netif/ethernet.c` + the adapter's CMSIS `sys_arch.c` (`port/adapters/lwip/`, `lwipopts.h` shadow config). Booted as the `tcpip` thread; the wlan netif bridge (`port/adapters/net80211/lwip/lwip_netif.c`) compiles on this line into the lwip world. The sequential API turned on with the netutils feat (LWIP_NETCONN/LWIP_SOCKET/LWIP_RAW/LWIP_DNS + `LWIP_SO_RCVTIMEO`, `MEMP_NUM_NETCONN`=10) — the BSD-socket surface iperf3_embedded and the netutils tools code against. Not compiled: `autoip.c`, `igmp.c`. |
| wpa_supplicant (RTOS port) | `github.com/KevinACoder/wpa_supplicant-rtos` (upstream of upstream: NXP `mcuxsdk-middleware-wireless-wpa_supplicant-rtos`; hostap `2.11-devel` base) | `0248409847056512eafae878a0a2260f10437b3c` | BSD-3 | submodule `third-party/wpa_supplicant` (working tree kept sparse: `src` + `wpa_supplicant`, top-level `LICENSE` rides the cone-mode root) | WPA2-PSK supplicant for the wlan line. Compiled set = the PSK-only list in the Makefile `WPA_CORE_SRCS` (rsn_supp/common/utils/crypto-internal/ap subset; no EAP, no TLS engine, no hostapd/P2P/ctrl-iface/SME). The OS/driver/eloop/l2_packet/main/cmd faces live in `port/adapters/wpa_supplicant/` and register as the `embox` driver the fork's `src/drivers/drivers.c` expects under `CONFIG_DRIVER_EMBOX`. One patch (`patches/wpa_supplicant/0001`: events.c roam comparisons in integer arithmetic, the `-mgeneral-regs-only` image keeps no FP state), materialized inside the submodule as branch `fwc/wpa_supplicant` via `make sync` (same discipline as `fwc/net80211`). |
| mpaland/printf | `github.com/mpaland/printf` | `d3b9846` (the repository's final state; upstream archived) | MIT | vendored `third-party/printf/` (2 files + LICENSE + `PROVENANCE.md`) | The libc formatting engine behind `port/aarch64/minilibc.c` (vsnprintf/snprintf/printf symbols stay in minilibc as wrappers; `_putchar` is minilibc's buffered console sink). Float/exponential compiled out at the Makefile rule (the `-mgeneral-regs-only` image keeps no FP state). Upstream is a real git home but the vendored form matches the tlsf precedent: the seam (who implements the standard names) lives in our libc file, and the upstream repo is frozen. |
| RT-Thread netutils | `github.com/RT-Thread-packages/netutils` | `b77ba3f` (`1.3.3-13`) | Apache-2.0 | vendored `third-party/netutils/` (per-component dirs + `PROVENANCE.md`; `tftp_port.c` NOT vendored) | The comprehensive-network-test line's tool set. Landed so far: `ping/` (unchanged) and the tftp core (`tftp_client/server/xfer`, unchanged - RTOS-free by design). The rt_* surface binds through the `port/adapters/netutils/shim/` shadow headers (rtthread.h/rtdbg.h/finsh.h/sys-socket+select onto ThreadX/CMSIS/mpaland-printf/CherrySH/lwIP-sockets; mapping table in the shim) and `netutils_shim.c`. The tftp port file is first-party (`port/adapters/netutils/tftp_port.c`): RAM-buffer file hooks (no filesystem on this trunk), CRC32 integrity print, CherrySH command. netio/ landed (one tracked tick-rate fix, see PROVENANCE); ntp/ was NOT vendored (GPL-2 header on ntp.h - first-party SNTP in `port/adapters/netutils/ntp_port.c`); telnet/ is NOT vendored (RT-Thread console-switch architecture; first-party cherrysh-multi-instance telnetd in `port/adapters/netutils/telnet_port.c`); tcpdump/ landed (transport reworked onto a pcap-over-TCP stream + netif_find + argv parser - deviations in PROVENANCE); upstream's `iperf/` is never ported (not protocol-compatible with real iperf - the carrier is `third-party/iperf3_embedded`). |

## Lineage of first-party code

All first-party code (`app/`, `drivers/`, `port/`, the `include/` gap
headers, `tools/`) is project-authored, BSD-2, carried from the frozen
pre-rework workspace (`~/backups/web_cmera_260925/freewebcamera`, branch
`wip/threadx-uc-gdb` @ `60b90e2`, a tested superset of `master` @ `abee7fd`)
and adapted for this trunk:

- **gdb stub × shell coexistence is new here.** The frozen UP carrier had no
  shell (D56: the two would fight over the one UART). This trunk arbitrates
  ownership instead: the shell owns RX; `tx_gdb_glue.c` claims the line at
  trap entry (`dbg_intr_ctrl(1)` = GIC-level mask of the console INTID, the
  CMSIS reception stays armed) and hands it back at session exit; the
  shell's RX path offers every 0x03 byte to `board_console_break_hook()`
  (weak seam in `board_early.c`, strong in the glue); the tick watcher only
  scans while the stub owns the line, so it never races the shell's ISR.
- Boot path is shell-first: `board_main` → console/banner/cntfrq → the
  anchors (ITS ladder, SPI soft-trigger) run in the shell-start task ahead
  of `shell_start()`. The old carrier's boot-time `gdb_break()` gate became
  the shell command `dbg`; the probe bodies (`dbg_probe_breakpoint/step`)
  stay in `app/dbg_scenario.c`.
- Trims vs the frozen branch (each returns with its own feat/xxx line):
  CherryUSB + xHCI, net80211 + urtwn/rtw8189f + wpa_supplicant, lwIP +
  iperf3, FatFs + sdmmc/eMMC, storage drivers (NVMe/AHCI/PCIe/SATA/MMC) and
  their board bindings, the FreeRTOS port, the kernel-replacement stub set
  (`make k4`), and the I2C/TSADC/SFC peripherals behind
  `port/adapters/periph/`.
- `port/aarch64/minilibc.c` grew a `printf` shim: the old link got one
  accidentally from the net80211 adapter's `wlan_console.c`; with that tree
  gone the seam is explicit in the libc home.
