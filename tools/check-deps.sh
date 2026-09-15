#!/bin/sh
# Dependency-direction gate (modularity check K2).
#
# Layer rule, enforced at the include path level:
#   app/ and drivers/  -> may include ONLY include/ (interface headers)
#   hal/, port/adapters/ -> may include third-party/ and the kernel
#   third-party/        -> must not include anything of ours
#
# A violation here means a layer boundary has been crossed: the caller now
# depends on a concrete component instead of the interface.

set -u

fail=0
root=$(dirname "$0")/..

say_ok()   { printf '  ok    %s\n' "$1"; }
say_fail() { printf '  FAIL  %s\n' "$1"; fail=1; }

printf 'check-deps\n'

# Anything that reveals a concrete component behind the interface layer.
#
# Matches both quote styles: a kernel header reached as <task.h> leaks the
# kernel just as surely as "task.h" does, and the include path is where that
# would come from. FreeRTOSConfig.h is included deliberately by name, so it is
# in the list too - a layer that has to know the kernel's configuration is no
# longer kernel-independent.
leaky='#include[[:space:]]*[<"](FreeRTOS\.h|FreeRTOSConfig\.h|task\.h|queue\.h|semphr\.h|event_groups\.h|timers\.h|stream_buffer\.h|list\.h|portmacro\.h|chry_ringbuffer\.h|chry_shell\.h|csh\.h)[>"]|#include[[:space:]]*[<"].*third-party'

# port/board/ is board-level bring-up: it must stay kernel-independent too,
# because the whole point of doing the tick through the CMSIS OS_Tick_* shape
# is that swapping the kernel does not require editing it.
for dir in app drivers port/board; do
    [ -d "$root/$dir" ] || continue
    hits=$(grep -rnIE "$leaky" "$root/$dir" 2>/dev/null)
    if [ -z "$hits" ]; then
        say_ok "$dir/ has no third-party or kernel includes"
    else
        say_fail "$dir/ includes concrete components (must go via include/):"
        printf '%s\n' "$hits"
    fi
done

# third-party/ must not depend on us.
if [ -d "$root/third-party" ]; then
    hits=$(grep -rnE '#include.*"(hal|include|port|app|drivers)/' "$root/third-party" 2>/dev/null | head -20)
    if [ -z "$hits" ]; then
        say_ok 'third-party/ does not include project headers'
    else
        say_fail 'third-party/ includes project headers (reverse dependency):'
        printf '%s\n' "$hits"
    fi
fi

printf 'check-deps: %s\n' "$([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
exit "$fail"
