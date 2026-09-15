#!/bin/sh
# Clean-room gate: fail if vendor traces or non-permissive licenses appear.
#
# Trace scan excludes third-party/ (vendored upstream, byte-identical) and
# docs/ (prose may explain provenance). See docs/clean-room.md in the parent
# workspace for the authoritative rule set.

set -u

fail=0

say_ok()   { printf '  ok    %s\n' "$1"; }
say_fail() { printf '  FAIL  %s\n' "$1"; fail=1; }

printf 'cleanroom-scan\n'

# --- 1. Vendor trace identifiers ------------------------------------------
traces=$(git grep -nIE \
  'phytium|Phytium|PHYTIUM|fparameters|fgic|FCache|FT_DEBUG|FT_[A-Z]|fdwgmac|fdwi2c|fdwmmc|fdwmshc|fdwpcie|fsata|fdsfc|fsdif|fxmac|fgmac|fpl011|f16550' \
  -- . ':!third-party' ':!docs' ':!.zcode' 2>/dev/null)
if [ -z "$traces" ]; then
    say_ok 'vendor trace scan (expect empty)'
else
    say_fail 'vendor trace scan found:'
    printf '%s\n' "$traces"
fi

# --- 2. Vendor-style filenames --------------------------------------------
# The 'f<lowercase>' pattern also matches innocent names (fifo.c, flash.c);
# review any hit by hand rather than assuming a false positive.
badnames=$(find . -path ./third-party -prune -o -path ./.git -prune -o \
  -type f \( -name 'f[a-z]*.c' -o -name 'f[a-z]*.h' -o -name 'f[a-z]*.S' \) -print 2>/dev/null)
if [ -z "$badnames" ]; then
    say_ok 'filename scan (expect empty)'
else
    say_fail 'filename scan found (review by hand):'
    printf '%s\n' "$badnames"
fi

# --- 3. Copyleft / license -------------------------------------------------
copyleft=$(git grep -ilE 'GNU (Lesser )?General Public License|SPDX-License-Identifier: *(L)?GPL' \
  -- . ':!third-party' 2>/dev/null)
if [ -z "$copyleft" ]; then
    say_ok 'copyleft scan (expect empty)'
else
    say_fail 'copyleft found:'
    printf '%s\n' "$copyleft"
fi

printf 'cleanroom-scan: %s\n' "$([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
exit "$fail"
