# Vendored upstream provenance

Every third-party component is vendored **byte-identical** to upstream. All
adaptation lives outside the vendored tree (in `port/adapters/`, `hal/`,
`include/` and shadow headers). Do not edit anything under `third-party/`.

Registering a component and creating its `port/adapters/<component>/`
directory are the same act — a component without an adapter is unfinished.

| Component | Upstream source | Revision | Version | License | Vendored at | Used for |
|---|---|---|---|---|---|---|
| FreeRTOS-Kernel | `github.com/FreeRTOS/FreeRTOS-Kernel` | `3a22924e0a9ddbbc8b0758881c33b3422a5cc20d` | V11.3.1 | MIT | `third-party/FreeRTOS-Kernel/` | Kernel + `portable/GCC/ARM_AARCH64_SRE` (carrier context-switch core) |
| CMSIS_6 | `github.com/ARM-software/CMSIS_6` | `26206e47dcf0abfbdc64eb753a0b6334b24439f6` | v6.3.1-dev-32 | Apache-2.0 | `third-party/cmsis/` | Interface layer: `RTOS2/Include/cmsis_os2.h` + `os_tick.h`, `Driver/Include/Driver_*.h` (18), `Core/Include/a-profile/irq_ctrl.h` |
| CherrySH | `github.com/cherry-embedded/CherrySH` | `8efe539c6e55b71d2f2cd1116cd00c4d64222a67` | v1.0.1-20 | Apache-2.0 | `third-party/cherrysh/` | Interactive shell |
| CherryRB | `github.com/cherry-embedded/CherryRB` | `19ea7c6efcf19dc9e805a0a662212533ee5b1edb` | v1.0.0 | Apache-2.0 | `third-party/cherryrb/` | Ring buffer (console input; later UVC/network/storage streams) |
| lwIP | `github.com/lwip-tcpip/lwip` | `77dcd25a72509eb83f72b033d219b1d40cd8eb95` | `STABLE-2_2_1_RELEASE` (2.2.1) | BSD-2 | `third-party/lwip/` | Networking: IPv4/ARP/ICMP/UDP/TCP core plus the FreeRTOS OSAL from upstream `contrib/ports/freertos/`. Vendored byte-identical (the whole `src/` tree, `contrib/ports/freertos/`, `COPYING`, `CHANGELOG`, `README`, `FILES`; verified with `diff -r` against the staging clone). The *compiled* subset is the Makefile's `LWIP_SRCS`; IPv6, PPP, altcp, sockets, DNS and the apps directories are not compiled (switched off in `port/adapters/lwip/include/lwipopts.h`). The httpd lands with M1-B. |
| FatFs | ChaN, from the lab's `web_camera/ff16.zip` (upstream package) | R0.16, `FF_DEFINED 80386` | R0.16 (2025-07-22) | ChaN FatFs licence (permissive, BSD-like) | `third-party/fatfs/` | File system over the block devices (M3). Vendored: `ff.c`, `ff.h`, `ffunicode.c`, `diskio.h` plus `LICENSE.txt`, `00readme.txt`, `00history.txt` — each extracted file's sha256 compared against the zip entry. **Not** vendored, by design: `ffconf.h` (upstream's copy would shadow the adapter's, since `ff.c` includes `"ffconf.h"` relative-first), `diskio.c` and `ffsystem.c` (upstream ships them as integrator skeletons — the adapter implements `diskio` and the OS hooks itself, see `port/adapters/fatfs/`). |
| fsl_sdmmc (SD/MMC/SDIO protocol layer) | licence holds from NXP's `github.com/nxp-mcuxpresso/mcuxsdk-middleware-sdmmc`; the **vendored bytes** come from the lab's own `os/standalone/third-party/fsl_sdmmc` (the standalone line's self-consistent copy — its protocol layer + host layer + dispatcher are one matched set, where NXP's own tree ships no host header for this controller) | NXP tree `aaafcf85b8eab09042ffcd503f7c96d0bdb1bd60` (v25.06.00); standalone copy as checked out at that tree's `297a4116` | v25.06.00 | BSD-3-Clause (NXP) | `third-party/sdmmc/{common,mmc,sd,osa,sdio}/` | SD / MMC / SDIO protocol stack (M3): card init, CMD/ACMD sequences, EXT_CSD, SDIO CCCR/FBR/CIS. `common/`, `mmc/`, `sd/`, `sdio/`, `osa/` only. **Not** vendored: NXP's `host/{usdhc,sdhc,sdif,spi}` (its own IP). The host layer for this board is the ported DW-MMC / DWC-MSHC driver pair plus the two host-glue files, whose upstream names and revisions are registered in `docs/imports.md` §2.2. The three NXP SDK headers the stack `#include`s by name (`fsl_common.h`, `fsl_os_abstraction.h`, `fsl_debug_console.h`) and `fsl_sdmmc_host.h`/`finterrupt.h` are shadowed in `port/adapters/sdmmc/shadow/`; the `osa/` implementation is our own on CMSIS-RTOS2 (`port/adapters/sdmmc/sdmmc_osa.c`). |

Notes on what was deliberately **not** vendored:

- **CMSIS_6 Core CPU/GIC implementation** (`irq_ctrl_gic.c`, `core_ca*.h`,
  `a-profile/gicv2.h`): GICv2 only — this board is a GIC-600 (GICv3). We take
  the `irq_ctrl.h` *API shape* and write our own GICv3 backend.
- **CMSIS_6 `RTOS2/Source/os_tick_*.c`**: they depend on `RTE_Components.h`,
  `CMSIS_device_header` and the `PL1_*` CMSIS-Core wrappers. Used as an
  architectural reference for the `OS_Tick_*` shape only.
- **CherrySH `builtin/lsusb.c`**: depends on CherryUSB headers, not yet in tree.
- **FreeRTOS-Kernel `portable/ThirdParty/`, CMake/SPDX extras**: unused.
- **lwIP `contrib/ports/{unix,win32}`**: not our OS. Only the FreeRTOS port is
  vendored. The vendor standalone SDK's own lwIP port tree was never a
  candidate either (vendor naming throughout it; the GMAC netif glue here is
  written against this project's CMSIS drivers — gap G2). The excluded tree is
  named in `docs/imports.md`, which is allowed to carry vendor names.

## Clean-room status

All vendored components are permissively licensed and carry **no vendor
traces**. Verify with `tools/cleanroom-scan.sh` (which excludes
`third-party/` from the trace scan, since vendored trees are byte-identical
upstream content).
