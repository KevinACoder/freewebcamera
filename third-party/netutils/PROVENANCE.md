# Provenance — RT-Thread netutils

- Upstream: https://github.com/RT-Thread-packages/netutils
- Version: `1.3.3-13-gb77ba3f` (master @ b77ba3f, "Merge PR #108")
- License: Apache-2.0 — see `LICENSE` (in-tree, upstream file)
- Form: **vendored, not a submodule** (NOTICE.md feat/netutils decision: the
  port to CMSIS-RTOS2/ThreadX is expected to be invasive, so the upstream
  pin + patches machinery would buy nothing — same reasoning as tlsf).

## Files vendored (and their deviations)

| File | Deviations from upstream |
|---|---|
| `ping/ping.c` | none |
| `tftp/tftp.h` | none |
| `tftp/tftp_client.c` | none |
| `tftp/tftp_server.c` | none |
| `tftp/tftp_xfer.c` | none |
| `tftp/tftp_xfer.h` | none |
| `tftp/tftp_port.c` | **NOT vendored** — it is the upstream port file (RTOS glue + dfs file hooks + msh CLI); this trunk's first-party port file lives at `port/adapters/netutils/tftp_port.c` (RAM-buffer file hooks, CRC32 integrity print, CherrySH command) |

## What binds the vendored code to this image

- `port/adapters/netutils/shim/` — shadow headers found first on the
  netutils compile world's include path: `rtconfig.h` (component switches,
  RT_TICK_PER_SECOND=1000), `rtthread.h` (rt_* surface mapped onto
  ThreadX/CMSIS/libc — the mapping table is documented there),
  `rtdbg.h` (LOG_* → session-routed printf), `finsh.h` (MSH_CMD_EXPORT →
  CherrySH `CSH_CMD_EXPORT_ALIAS_FULL`), `sys/socket.h`/`sys/select.h`
  (BSD socket surface → lwIP sockets).
- `port/adapters/netutils/netutils_shim.c` — the rt_* realizations.
- The BSD socket calls resolve against lwIP 2.2.1 (LWIP_SOCKET=1,
  LWIP_COMPAT_SOCKETS=1, LWIP_TIMEVAL_PRIVATE=0 so `struct timeval` comes
  from the toolchain's `<sys/time.h>` via `arch/cc.h`).
- malloc/free/calloc/realloc resolve to the image's TLSF-backed libc names.

## Not ported (this feat)

- `iperf/` — RT-Thread's own iperf is not protocol-compatible with real
  iperf2/iperf3; this line's iperf carrier is `third-party/iperf3_embedded`
  (real iperf3 protocol, board-validated against esnet iperf 3.19.1).
- `netio/`, `ntp/`, `telnet/`, `tcpdump/` — each lands in its own milestone
  of the netutils feat (see the feat issue for the adaptation notes).
