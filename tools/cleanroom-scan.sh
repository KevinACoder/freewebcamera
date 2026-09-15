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
# The scan script itself is excluded: it necessarily contains the vendor
# tokens, as data, and `git grep` would otherwise always match its own pattern
# list and fail every run.
traces=$(git grep -nIE \
  'phytium|Phytium|PHYTIUM|fparameters|fgic|FCache|FT_DEBUG|FT_[A-Z]|fdwgmac|fdwi2c|fdwmmc|fdwmshc|fdwpcie|fsata|fdsfc|fsdif|fxmac|fgmac|fpl011|f16550' \
  -- . ':!third-party' ':!docs' ':!.zcode' ':!tools/cleanroom-scan.sh' 2>/dev/null)
if [ -z "$traces" ]; then
    say_ok 'vendor trace scan (expect empty)'
else
    say_fail 'vendor trace scan found:'
    printf '%s\n' "$traces"
fi

# --- 2. Vendor-style filenames --------------------------------------------
# The 'f<lowercase>' pattern also matches innocent names (fifo.c, flash.c);
# review any hit by hand rather than assuming a false positive.
# build/ and _baseline/ are generated/archived output, not sources: scanning
# them reports objects and backup images, not code we wrote.
badnames=$(find . -path ./third-party -prune -o -path ./.git -prune -o \
  -path ./build -prune -o -path ./_baseline -prune -o \
  -type f \( -name 'f[a-z]*.c' -o -name 'f[a-z]*.h' -o -name 'f[a-z]*.S' \) -print 2>/dev/null)
if [ -z "$badnames" ]; then
    say_ok 'filename scan (expect empty)'
else
    say_fail 'filename scan found (review by hand):'
    printf '%s\n' "$badnames"
fi

# --- 3. Copyleft / license -------------------------------------------------
# docs/ is excluded: prose legitimately names licenses when explaining the
# rule set. This script is excluded for the same reason as in check 1.
copyleft=$(git grep -ilE 'GNU (Lesser )?General Public License|SPDX-License-Identifier: *(L)?GPL' \
  -- . ':!third-party' ':!docs' ':!tools/cleanroom-scan.sh' 2>/dev/null)
if [ -z "$copyleft" ]; then
    say_ok 'copyleft scan (expect empty)'
else
    say_fail 'copyleft found:'
    printf '%s\n' "$copyleft"
fi

printf 'cleanroom-scan: %s\n' "$([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
exit "$fail"
