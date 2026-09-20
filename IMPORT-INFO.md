# Vendored upstream provenance

Every third-party component is vendored **byte-identical** to upstream. All
adaptation lives outside the vendored tree (in `port/adapters/`, `hal/`,
`include/` and shadow headers). Do not edit anything under `third-party/`.

Registering a component and creating its `port/adapters/<component>/`
directory are the same act — a component without an adapter is unfinished.

**Registered local deviations from byte-identical** (clean-room §4.1:
minimal, marked `FREERTOS_PORT:` at the site, registered here and in
`docs/imports.md` in the same commit):

| Component | File | Deviation | Why |
|---|---|---|---|
| FreeRTOS-Kernel V11.3.1 | `tasks.c` `prvYieldCore` | Two statements swapped: `xTaskRunState = taskTASK_SCHEDULED_TO_YIELD` now executes **before** `portYIELD_CORE()`, with an inline `FREERTOS_PORT:` comment. | Upstream order raises the cross-core IPI first; the target core's yield ISR can reselect the same task and clear the run state to RUNNING before the flag store lands, leaving the late flag with nobody to clear it — the task then waits forever in `prvCheckForRunStateChange`. Observed on hardware (pinned delayed tasks never woke; the reference SMP line carries the identical fix). |
| port (transplanted SMP port) | `port.c` `ullCriticalNesting[]` | SMP array seeded 0 (reference: 9999). | Vestigial under the SMP kernel (nesting lives in the TCB); 9999 makes the asm restore path park ICC_PMR at the API level forever, starving FromISR drivers that sit at that level. |
| port (transplanted SMP port) | `port.c` `uxPortSetInterruptMask` / `portUMASK_INTERRUPT` | No blind `portENABLE_INTERRUPTS`/DAIFCLR; caller's DAIF saved and restored exactly. | FromISR paths enter with IRQs masked by exception entry; a blind re-enable lets level-triggered lines nest into their own critical section (the wip/smp-qemu line carried and board-proved the same fix). |
| port (transplanted SMP port) | `port.c` `icc_rpr_read` | ICC_RPR_EL1 encoded `s3_0_c12_c11_3`. | First draft used `s3_0_c12_c8_0` = ICC_IAR0 (Group 0); an NS EL1 read traps to EL3 and OP-TEE's unexpected-trap path resets the machine silently. Board-proven (5-round freeze root cause). |
| board | `gicv3.c` `board_gicv3_send_sgi` | SGI1R encoded `s3_0_c12_c11_5` (was `_6` = ICC_ASGI1R). | Per ARM IHI 0069 and the standalone reference tree's GIC header; ASGI1R has a different targeting semantic. |
| FreeRTOS test suite | `tests/test_runner.c` | Dropped the template's `<stdio.h>`, `<stdlib.h>` and `"flop.h"` includes (marked `FREERTOS_PORT(tests):`). | The image is freestanding (`-nostdlib`, nothing uses them); the FPU test is out of scope (`-mgeneral-regs-only`) and `flop.h` trips the clean-room f-prefix filename gate — its `configSTART_MATH_TESTS` switch stays 0 so the guarded blocks preprocess away. |
| FreeRTOS test suite | `tests/minimal/IntQueue.c` | (a) the two "high-priority task must be `eSuspended` at this instant" checks wrapped `#if ( configNUMBER_OF_CORES == 1 )`; (b) `xSecondTimerHandler`'s four paired queue accesses wrapped in `taskENTER/EXIT_CRITICAL_FROM_ISR` under SMP. | (a) observed from an ISR, the instantaneous scheduling state races across cores and would false-fail (the reference SMP line's board adaptation, adopted after diffing all 22 suites against it); (b) both timer handlers can run concurrently on different cores, so the paired accesses must be one atomic group for the test's value accounting to hold. |

| Component | Upstream source | Revision | Version | License | Vendored at | Used for |
|---|---|---|---|---|---|---|
| FreeRTOS-Kernel | `github.com/FreeRTOS/FreeRTOS-Kernel` | `3a22924e0a9ddbbc8b0758881c33b3422a5cc20d` | V11.3.1 | MIT | `third-party/FreeRTOS-Kernel/` | Kernel. M0 compiled the upstream single-core `portable/GCC/ARM_AARCH64_SRE` port directly; since D33 the compiles-come-from port is the transplanted SMP port (`port/adapters/freertos/{portmacro.h,port.c,portasm_smp.S,port_vectors.S}`, logic-faithful from the board-validated reference SDK line, registered in `docs/imports.md` §1.1 with the deviation table above). Both the single-core SRE port and the retired project-owned SMP port remain in git history only. |
| FreeRTOS test suite | `github.com/FreeRTOS/FreeRTOS` (monorepo) | `f4fcc3b228643144727e9257ba12db1cb632b6e6` (main, 2026-08-26) | V202212.00 demo headers | MIT | `port/adapters/freertos/tests/` | Kernel feature/stress tests (D34, `make ktest`): the official `Demo/ThirdParty/Template/TestRunner.{c,h}` + `IntQueueTimer.h` + `RegTests.h`, 22 `Demo/Common/Minimal` suites and 28 `Demo/Common/include` headers copied verbatim (deviations in the table above); `tests/intqueue_timer.c` implements the template's board contract (second interrupt source) on a software-pended SPI; `tests/tests_config.h`, `tests/ktest_support.c` and `app/ktest_main.c` are project-owned. Only the ktest image links these. |
| CMSIS_6 | `github.com/ARM-software/CMSIS_6` | `26206e47dcf0abfbdc64eb753a0b6334b24439f6` | v6.3.1-dev-32 | Apache-2.0 | `third-party/cmsis/` | Interface layer: `RTOS2/Include/cmsis_os2.h` + `os_tick.h`, `Driver/Include/Driver_*.h` (18), `Core/Include/a-profile/irq_ctrl.h` |
| CherrySH | `github.com/cherry-embedded/CherrySH` | `8efe539c6e55b71d2f2cd1116cd00c4d64222a67` | v1.0.1-20 | Apache-2.0 | `third-party/cherrysh/` | Interactive shell |
| CherryRB | `github.com/cherry-embedded/CherryRB` | `19ea7c6efcf19dc9e805a0a662212533ee5b1edb` | v1.0.0 | Apache-2.0 | `third-party/cherryrb/` | Ring buffer (console input; later UVC/network/storage streams) |
| lwIP | `github.com/lwip-tcpip/lwip` | `77dcd25a72509eb83f72b033d219b1d40cd8eb95` | `STABLE-2_2_1_RELEASE` (2.2.1) | BSD-2 | `third-party/lwip/` | Networking: IPv4/ARP/ICMP/UDP/TCP core plus the FreeRTOS OSAL from upstream `contrib/ports/freertos/`. Vendored byte-identical (the whole `src/` tree, `contrib/ports/freertos/`, `COPYING`, `CHANGELOG`, `README`, `FILES`; verified with `diff -r` against the staging clone). The *compiled* subset is the Makefile's `LWIP_SRCS`; IPv6, PPP, altcp, sockets, DNS and the apps directories are not compiled (switched off in `port/adapters/lwip/include/lwipopts.h`). The httpd lands with M1-B. |
| FatFs | ChaN, from the lab's `web_camera/ff16.zip` (upstream package) | R0.16, `FF_DEFINED 80386` | R0.16 (2025-07-22) | ChaN FatFs licence (permissive, BSD-like) | `third-party/fatfs/` | File system over the block devices (M3). Vendored: `ff.c`, `ff.h`, `ffunicode.c`, `diskio.h` plus `LICENSE.txt`, `00readme.txt`, `00history.txt` — each extracted file's sha256 compared against the zip entry. **Not** vendored, by design: `ffconf.h` (upstream's copy would shadow the adapter's, since `ff.c` includes `"ffconf.h"` relative-first), `diskio.c` and `ffsystem.c` (upstream ships them as integrator skeletons — the adapter implements `diskio` and the OS hooks itself, see `port/adapters/fatfs/`). |
| fsl_sdmmc (SD/MMC/SDIO protocol layer) | licence holds from NXP's `github.com/nxp-mcuxpresso/mcuxsdk-middleware-sdmmc`; the **vendored bytes** come from the lab's own `os/standalone/third-party/fsl_sdmmc` (the standalone line's self-consistent copy — its protocol layer + host layer + dispatcher are one matched set, where NXP's own tree ships no host header for this controller) | NXP tree `aaafcf85b8eab09042ffcd503f7c96d0bdb1bd60` (v25.06.00); standalone copy as checked out at that tree's `297a4116` | v25.06.00 | BSD-3-Clause (NXP) | `third-party/sdmmc/{common,mmc,sd,osa,sdio}/` | SD / MMC / SDIO protocol stack (M3): card init, CMD/ACMD sequences, EXT_CSD, SDIO CCCR/FBR/CIS. `common/`, `mmc/`, `sd/`, `sdio/`, `osa/` only. **Not** vendored: NXP's `host/{usdhc,sdhc,sdif,spi}` (its own IP). The host layer for this board is the ported DW-MMC / DWC-MSHC driver pair plus the two host-glue files, whose upstream names and revisions are registered in `docs/imports.md` §2.2. The three NXP SDK headers the stack `#include`s by name (`fsl_common.h`, `fsl_os_abstraction.h`, `fsl_debug_console.h`) and `fsl_sdmmc_host.h`/`finterrupt.h` are shadowed in `port/adapters/sdmmc/shadow/`; the `osa/` implementation is our own on CMSIS-RTOS2 (`port/adapters/sdmmc/sdmmc_osa.c`). |
| CherryUSB | `github.com/cherry-embedded/CherryUSB` | `0e40349b4b5615f019f9d34c1ac92a7b23433a94` (master, 2026-09-18) | master (post v1.6.1) | Apache-2.0 | USB host stack (M6-A, enumeration slice). Vendored byte-identical from a fresh clone at the locked commit: `core/usbh_core.{c,h}`, `class/hub/{usbh_hub.c,usbh_hub.h,usb_hub.h}`, `port/ehci/{usb_hc_ehci.c,usb_hc_ehci.h,usb_ehci_reg.h,README.md}`, `osal/usb_osal_freertos.c`, the thirteen `common/*.h`, `LICENSE`, `VERSION`. Not vendored: the device-side cores, every other class (`msc`/`hid` land with the M6 close-out, `video` with M8), all glue files (the adapter is the glue), and the vendor xHCI/ohci ports. `usb_config.h` is the adapter's shadow header. |
| Eclipse ThreadX (SMP kernel) | `github.com/eclipse-threadx/threadx` | `44d7c95c` (`v6.5.1.202602a_rel-1-g44d7c95c`, 2026-09) | 6.5.1.202602a | MIT | `third-party/threadx/` | Second kernel for the SMP comparison line (D39): the board problem "4-core shell unresponsive" (20260919 issue) is bisected by running the identical console/shell path on a different SMP kernel. Vendored byte-identical (verified with `diff -r` against the staging clone): `common_smp/{src,inc}` (the SMP kernel source set; the UP `common/` set is a different source selection and is **not** vendored) and `ports_smp/cortex_a55_smp/gnu/{src,inc}` (the ARMv8-A 4-core GNU port). **Not** vendored, by design: `example_build/` (FVP-specific: GICD@0x2f000000, SP804 tick, EL3 startup, no PSCI — every line of it is wrong for this board), all other architecture ports, `common/` (UP kernel), CMake/samples/test scaffolding. The port talks to the board only through the vector-table entries in `port/adapters/threadx/tx_vectors.S` and the BSP hooks in `tx_glue.c`; its GIC/timer/startup expectations are met by this project's kernel-agnostic board layer, never by upstream example code. |

Notes on what was deliberately **not** vendored:

- **CMSIS_6 Core CPU/GIC implementation** (`irq_ctrl_gic.c`, `core_ca*.h`,
  `a-profile/gicv2.h`): GICv2 only — this board is a GIC-600 (GICv3). We take
  the `irq_ctrl.h` *API shape* and write our own GICv3 backend.
- **CMSIS_6 `RTOS2/Source/os_tick_*.c`**: they depend on `RTE_Components.h`,
  `CMSIS_device_header` and the `PL1_*` CMSIS-Core wrappers. Used as an
  architectural reference for the `OS_Tick_*` shape only.
- **CherrySH `builtin/lsusb.c`**: depends on CherryUSB headers (which are in
  tree since M6-A), but the adapter ships its own `usbh` tree command instead;
  the builtin stays excluded to keep the shell's vendored surface unchanged.
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

The SMP port (`port/adapters/freertos/portmacro.h`, `port_smp.c`,
`portasm_smp.S`) is **project-owned**, derived from the vendored upstream
MIT port (`portable/GCC/ARM_AARCH64_SRE`) as that licence allows; each file
documents what was kept and what was added. A vendor SMP SDK was consulted
for mechanism only — secondary boot via PSCI, SGI yield, per-core GICR
walk, single-tick model — and no code was taken from it; the tree is named
in `docs/imports.md` §2 (the only place vendor names belong), and the
derivation is registered in §1.1.
