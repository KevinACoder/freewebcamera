# IMPORT-INFO.md — third-party provenance

Trunk-baseline dependency policy (NOTICE.md): every component with a real
upstream git home is a **pinned git submodule**; any local deviation lives in
`patches/<component>/` (today: the four `net80211` patches — see
`patches/README.md`). Components
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
| NetBSD net80211 + USB host stack | `github.com/KevinACoder/netbsd-src` (branch `netbsd-11`; upstream of upstream: `github.com/NetBSD/src`) | `111525e242df83d65ff69e7bcbde03e141ce3e08` | BSD-2/ISC/BSD-3 family (per-file, NetBSD Foundation & contributors) | submodule `third-party/net80211` (recorded URL = local path `/home/zhugy/workspace/rk3568_lab/os/netbsd/src`; local-path submodule requires `git config protocol.file.allow always` — the `modules` target passes it via `-c`) | The wireless + USB host feat line: compiled subset is `sys/net80211` (28-file set, minus acl/tkip/wep/ioctl/rssadapt, per the frozen imports.md line), `sys/dev/usb` core (usbdi/usbdi_util/usb_mem/usb_subr/usb/usb_quirks/usbdevs/uhub/ehci) and `sys/dev/usb/if_urtwn.c`. The submodule's working tree is kept sparse to those subtrees + `external/realtek/urtwn` (firmware dist). All kernel shims live in `port/adapters/net80211/` — the submodule stays byte-identical to the pin; deviations go through `patches/net80211/` (four: sysctl compile-out, aes-ccm mbuf order, usbdi miss-out-ack log, urtwn cmd-ring overflow keeps the oldest command). The USB debug/history world is wired in by force-including the compat `opt_usb.h` (`USB_DEBUG`/`USB_DEBUG_DEFAULT`/`EHCI_DEBUG`, history ring 4096 records — the upstream 50000 is ~3 MB of .bss). |
| lwIP | `github.com/lwip-tcpip/lwip` | `77dcd25a72509eb83f72b033d219b1d40cd8eb95` (`STABLE-2_2_1_RELEASE`) | BSD-3 | submodule `third-party/lwip` | Network stack: `src/core`, `src/core/ipv4`, `src/api`, `netif/ethernet.c` + the adapter's CMSIS `sys_arch.c` (`port/adapters/lwip/`, `lwipopts.h` shadow config). Booted as the `tcpip` thread on this line; the wlan netif bridge returns with the association feat. |

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
