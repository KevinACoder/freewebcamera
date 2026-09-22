# iperf3_embedded

- Source: https://github.com/sqqdfny/iperf3_embedded
- Revision: `66614e4` (2026-06-17, "first commit")
- License: MIT (`LICENSE`, Copyright (c) 2024 iperf3-embedded contributors;
  the GitHub repository-level LICENSE says "(c) 2026 sqqdfny")
- Files: `iperf3_embedded.c`, `iperf3_embedded.h`, `LICENSE`
- Upstream scope: iperf3-protocol throughput client for lwIP (TCP + UDP,
  forward + reverse), server role not implemented upstream; the PC peer is
  a standard `iperf3 -s` (default port 5201)
- Upstream dependencies: lwIP BSD socket API (`LWIP_SOCKET`), a small
  FreeRTOS surface (task create/delete/delay/tick), two integrator-provided
  platform functions, `IPERF3_PRINTF`-family output macros

## Local adjustments (registered)

1. The FreeRTOS include block (`FreeRTOS.h` / `task.h`) is replaced by
   `#include "iperf3_port.h"` - the ThreadX SMP mapping lives in
   `port/adapters/lwip/iperf3_port.{h,c}` (CMSIS-RTOS2, CNTVCT time).
2. `#include <errno.h>` removed: this freestanding image has no newlib
   reent; `lwip/errno.h` (`LWIP_PROVIDE_ERRNO`) provides the constants and
   the variable instead, and the adapter defines the variable once
   (`port/adapters/lwip/lwip_diag.c`).

Everything else is upstream-identical; compare against the upstream commit
with `diff -r` (the `web_camera/iperf3_embedded/` clone tracks it).
