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

Notes on what was deliberately **not** vendored:

- **CMSIS_6 Core CPU/GIC implementation** (`irq_ctrl_gic.c`, `core_ca*.h`,
  `a-profile/gicv2.h`): GICv2 only — this board is a GIC-600 (GICv3). We take
  the `irq_ctrl.h` *API shape* and write our own GICv3 backend.
- **CMSIS_6 `RTOS2/Source/os_tick_*.c`**: they depend on `RTE_Components.h`,
  `CMSIS_device_header` and the `PL1_*` CMSIS-Core wrappers. Used as an
  architectural reference for the `OS_Tick_*` shape only.
- **CherrySH `builtin/lsusb.c`**: depends on CherryUSB headers, not yet in tree.
- **FreeRTOS-Kernel `portable/ThirdParty/`, CMake/SPDX extras**: unused.

## Clean-room status

All four vendored components are permissively licensed and carry **no vendor
traces**. Verify with `tools/cleanroom-scan.sh` (which excludes
`third-party/` from the trace scan, since vendored trees are byte-identical
upstream content).
