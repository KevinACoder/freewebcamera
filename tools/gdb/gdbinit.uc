# gdbinit.uc - ThreadX UP debug carrier (D56/D57), WSL xpack aarch64 gdb 13.2
#
#   aarch64-none-elf-gdb -x tools/gdb/gdbinit.uc
#
# Adjust BRIDGE/ELF if the build moved. Windows-side xpack gdb works the
# same way with file \\wsl.localhost\<distro>\home\...\threadx-uc.elf and
# target remote 127.0.0.1:18000 (or the host IP).

set confirm off
set pagination off
set architecture aarch64
set breakpoint pending on

# The image path carries the configuration name (configs/): every config
# builds into its own directory, so point this at the one you booted.  The
# default below is CONFIG=full; `make show-config` prints the path for any
# other config (e.g. build/rk3568-threadx-uc-min/threadx-uc.elf).
#
# The image is -mgeneral-regs-only: the target description declares only
# the 34 core registers; do not expect FP registers.
file build/rk3568-threadx-uc-full/threadx-uc.elf

# Breakpoint classes on this board (D57 triage): the boot chain (BL31 +
# OP-TEE + U-Boot) never delivers self-hosted debug events other than BRK
# - Z0 (BRK patch) works, Z1 (DBGBCR) and watchpoints hold their register
# values but their events stay silent. Keep the hw limits at 0 so `break`
# uses Z0; flip them up to exercise the DBGBCR/DBGWCR path (boot log
# reports 6 breakpoints / 1 watchpoint from ID_AA64DFR0).
set remote hardware-breakpoint-limit 0
set remote hardware-watchpoint-limit 0
set breakpoint auto-hw on

# oslab hub raw bridge (RW port). Session discipline: the WebUI serial
# terminal and MCP serial_read are evidence channels - they still work, but
# their ring view interleaves RSP with board logs.
target remote 192.168.0.18:18000

# Platform notes (D57):
# - The carrier builds -Og -g3, so `break func` lands on a reachable
#   address; `break *addr` remains the exact-placement discipline.
# - `break` is a patched BRK #0x401 (stop reason "swbreak"); the
#   compiled-in gdb_break()/Ctrl-C path uses the same immediate.
# - Single-step (`stepi`/`nexti`) does not deliver on this boot chain:
#   the SoftwareStep event stays silent. Use breakpoints instead.
# - Watchpoints need the DBGWCR event class - silent here too.
# - Any synchronous fault stops in the stub (SIGSEGV/SIGBUS/SIGILL by ESR
#   class) - there is no fatal park on this carrier; continue re-executes
#   the faulting instruction.
