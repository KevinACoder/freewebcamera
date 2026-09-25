# Provenance — mpaland/printf

- Upstream: https://github.com/mpaland/printf
- Version: d3b9846 ("chore(readme): 2020 announcement" — the repository's
  final state; upstream is archived and stable)
- License: MIT — see `LICENSE` (in-tree, upstream file)
- Files: `printf.c`, `printf.h` — **unmodified**
- Bind: the libc formatter seam lives in `port/aarch64/minilibc.c`, which
  declares the underscore realizations (`vsnprintf_`, …) directly —
  deliberately NOT including `printf.h`, whose standard-name `#define`
  aliases are unconditional and would collide with the standard-named
  symbols minilibc keeps as wrappers. `_putchar` (the `printf_`/`vprintf_`
  sink) is implemented in minilibc.c with a 256-byte buffer drained through
  `board_console_write`.
- Build config (Makefile, dedicated rule): `PRINTF_DISABLE_SUPPORT_FLOAT`
  and `PRINTF_DISABLE_SUPPORT_EXPONENTIAL` — the image is
  `-mgeneral-regs-only` (no FP state exists), so `%f`/`%e`/`%g` are compiled
  out and degrade like unknown specifiers (the bare specifier character is
  emitted). Long-long and ptrdiff support stay on (pure integer paths).
- Rationale: the previous first-party formatter was deliberately a subset
  (no precision, no `*` width, no `%.*s`) and bit the wpa line twice
  (`wpa status` ssid/bssid, `%.4x` in net80211 diagnostics). The netutils
  line ports RT-Thread code that assumes a fuller libc formatter, so the
  engine is upgraded wholesale rather than extended piecemeal.
