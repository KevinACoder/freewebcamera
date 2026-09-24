# gdbinit.uc - ThreadX UP debug carrier (D56), WSL xpack aarch64 gdb 13.2
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

# This image is -mgeneral-regs-only: the target description declares only
# the 34 core registers; do not expect FP registers.
file build/rk3568-threadx-uc/threadx-uc.elf

# oslab hub raw bridge (RW port). Session discipline: the WebUI serial
# terminal and MCP serial_read are evidence channels - they still work, but
# their ring view interleaves RSP with board logs.
target remote 192.168.0.18:18000

# Platform notes (board-proven, D56):
# - Software breakpoints are patched BRK #0x401; always prefer exact
#   addresses: gdb's line-table placement on this -O2 image can put
#   `break func` inside the *previous* function. Use:
#       break *net_start
# - Hardware breakpoints / single-step do not fire on this board (debug
#   register + maintenance constraint); `stepi` is best-effort.
# - Any synchronous fault stops in the stub (SIGSEGV/SIGBUS/SIGILL by ESR
#   class) - there is no fatal park on this carrier; continue re-executes
#   the faulting instruction.
