# Import information

All files under `net80211/` and `driver/` are verbatim copies (byte
identical, verified with cmp) from the NetBSD netbsd-11 sources.

- Source repository: https://github.com/KevinACoder/netbsd-src (fork of
  the NetBSD CVS tree, branch netbsd-11)
- Tree revision at import: `dea823526f92de8fb73b1a56b89f44b60a0f1637`
  (2026-09-07)
- Last per-directory commits at import time:
  - `sys/net80211/`: `95093fc595ba1f882f72b370c4abc23753ac8035`
  - `sys/dev/usb/if_urtwn.c`: `9a78d88900a4793bd333a709b420c85050dd09df`

## Mapping

| Import target | Upstream path |
|---|---|
| `net80211/*.c`, `net80211/*.h` | `sys/net80211/*` |
| `driver/urtwn/if_urtwn.c` | `sys/dev/usb/if_urtwn.c` |
| `driver/urtwn/if_urtwnreg.h` | `sys/dev/usb/if_urtwnreg.h` |
| `driver/urtwn/if_urtwnvar.h` | `sys/dev/usb/if_urtwnvar.h` |
| `driver/urtwn/rtwnreg.h` | `sys/dev/ic/rtwnreg.h` |
| `driver/urtwn/rtwn_data.h` | `sys/dev/ic/rtwn_data.h` |
| `firmware/urtwn/rtl8188eufw.bin` | `external/realtek/urtwn/dist/rtl8188eufw.bin` |

The upstream `sys/net80211/CHANGES`, `Makefile` and `files.net80211`
are not imported: they belong to the NetBSD build system.

## Rules for changes

- Imported files stay byte-identical whenever possible. The compiler is
  pointed at `compat/netbsd/` shadow headers through the include path
  (and, where unavoidable, a forced include of `port/port_config.h`)
  instead of editing the sources.
- If an import file must be touched, keep the edit minimal, mark it
  with a `/* NET80211_PORT(L): ... */` comment, and record it here.

## Library clean-up (2026-09-22)

The repository no longer vendors any third-party source tree; the OS,
USB and network-stack dependencies are provided by the integrator:

- `third-party/` (FreeRTOS V11.3.1 subset, lwIP 2.2.1) - removed; lwIP
  is provided by the integrator, and the FreeRTOS port was superseded
  by `port/osal/cmsis_rtos2/`.
- `port/bus/usb/cherryusb/cherryusb/` (CherryUSB 1.6.1, commit
  `1fd876d0`, with three local patches recorded in
  `port/bus/usb/cherryusb/IMPORT-INFO.md`) - removed; the CherryUSB
  headers are an external dependency of the USB backends.
- `port/rk3568/` (RK3568 bare-metal BSP, Phytium standalone SDK
  derived) - removed; integrators bring their own board support.
- `port/osal/freertos/`, the FreeRTOS variants of the USB/PCIe glue
  (`usb_glue_rk3568_freertos.c`, `net_bridge_freertos.c`,
  `pcie_freertos.c`), `port/wpa/` (the FreeRTOS wpa_supplicant port)
  and `port/utility/cherrysh/` (the FreeRTOS test shell) - removed;
  that glue lives with the integrator now.
- `port/Makefile` (the RK3568 FreeRTOS demo build) - removed together
  with the BSP it drove; the host tests under `tests/` remain the
  in-repo verification.
- Added `port/osal/cmsis_rtos2/` and the OS-agnostic
  `port/osal/wlan_port_core.h`; the registry TUs
  (`urtwn_reg.c`, `iwm_reg.c`, `usbh_urtwn_class.c`) now include the
  core header instead of a per-OS internal header.

## Compat additions for the freewebcamera integration (2026-09-22)

Gaps the freewebcamera build (newlib headers, -ffreestanding) exposed,
all in the self-authored shadow layer; the verbatim imports stay
byte-identical:

- `compat/netbsd/dev/ic/wi_ieee.h` - added, verbatim from the NetBSD
  tree (`sys/dev/ic/wi_ieee.h` rev 1.24, BSD license); the imported
  `ieee80211_ioctl.c` includes it and the embox lane used its own
  tree's copy.
- `compat/netbsd/compat/sys/sockio.h` - forwarder to `<sys/sockio.h>`;
  the imported `ieee80211_ioctl.c` spells the include as
  `<compat/sys/sockio.h>` (the NetBSD tree's own compat layout).
- `compat/netbsd/sys/time.h` - `struct timespec`/`timeval` now reuse
  newlib's internal guard names (`_SYS__TIMESPEC_H_`,
  `_SYS__TIMEVAL_H_`) so the compat structs and the libc's cannot be
  double-defined in one translation unit, whichever includes first.
- `port/port_config.h` - `PRIu64` defined (freestanding newlib's
  inttypes.h leaves the PRI macros out); uint64_t is unsigned long on
  this ABI.
- `port/port_config_bsd.h` - pulls the compat `sys/time.h` so the
  imported files see `struct timeval` without NetBSD's param.h chain.
