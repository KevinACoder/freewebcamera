# Build for the freewebcamera trunk baseline image:
#   ThreadX UP (single core) + serial gdb stub + cherrysh console.
#
# Produces a flat raw binary. The RK3568 board loads it with
#   tftp 0xa000000 freertos.bin ; go 0xa000000
# (bootelf is unusable on that board - it faults at the jump).
#
# Layers, and the include-path rule that enforces them:
#   include/      interface layer: vendored CMSIS headers + our gap headers
#   port/aarch64/ CPU bring-up (startup, MMU, GICv3/ITS, tick) + the gdb stub
#   port/board/   board_conf.h + link script, selected by BOARD=
#   port/adapters/ one directory per vendored component
#   drivers/      CMSIS driver implementations
#   app/          application
#   third-party/  upstream, pinned git submodules (+ patches/, see IMPORT-INFO.md)
#
# app/ and drivers/ never get third-party/ or kernel include paths: if they
# need something it has to come through include/. tools/check-deps.sh checks
# the same rule statically.

# Board selection: port/board/<board>/ supplies board_conf.h (register bases,
# interrupt numbers, MMU windows) and the link script.
BOARD ?= rk3568

# Bare-metal toolchain, set explicitly (non-interactive shells do not source
# ~/.bashrc - silently picking up a Linux-targeted compiler links against
# glibc assumptions that cannot work here).
AARCH64_CROSS_PATH ?= $(HOME)/workspace/env/src/xpack-aarch64-none-elf-gcc-13.2.1-1.1
CROSS_COMPILE ?= $(AARCH64_CROSS_PATH)/bin/aarch64-none-elf-
CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size

BUILD   := build/$(BOARD)-threadx-uc
TARGET  := $(BUILD)/threadx-uc

# Debug-carrier optimization profile (D57): symbols plus near-no optimization,
# so gdb's line table places breakpoints on addresses code actually reaches.
# UC_OPT=-O0 reproduces the reference SDK's CONFIG_DEBUG_NOOPT exact-noopt shape.
UC_OPT ?= -Og

# -mgeneral-regs-only: the port saves no FP state, so any FP instruction is a
#   bug we want the compiler to prevent rather than discover as corruption.
# -DGUEST/-DEL1: the port runs at EL1 (SPSR/ELR selection in the context-switch
#   assembly; CurrentEL asserts in the board layer).
# -mno-outline-atomics: a -nostdlib image has no library for __aarch64_*
#   helpers; inlined LDXR/STXR loops need none.
# -DTX_INCLUDE_USER_DEFINE_FILE: the kernel reads the adapter's tx_user.h.
CFLAGS := $(UC_OPT) -g3 -std=c11 -Wall -Wextra \
	-ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-march=armv8-a -mgeneral-regs-only -mstrict-align -mno-outline-atomics \
	-DGUEST -DEL1 -DSMP_CORES=1 \
	-DTHREADX_BUILD=1 -DTHREADX_UP_BUILD=1 \
	-DTX_INCLUDE_USER_DEFINE_FILE

LDFLAGS = -nostdlib -static -T port/board/$(BOARD)/$(BOARD).ld \
	-Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map=$(TARGET).map

# --- include paths -----------------------------------------------------------

INC_COMMON := -Iinclude -Iport/board -Iport/aarch64 -Iport/board/$(BOARD) -Idrivers

# Adapters only: they are the only layer allowed to touch vendored code and
# kernel internals. cmsis_os2_ext.h (osThreadFlagsSetFromISR) lives with the
# ThreadX CMSIS twin - it is the only kernel on this trunk.
INC_ADAPTER := -Ithird-party/threadx/common/inc \
	-Ithird-party/threadx/ports/cortex_a55/gnu/inc \
	-Iport/adapters/threadx \
	-Iport/adapters/cmsis_rtos2_threadx \
	-Ithird-party/tlsf \
	-Iport/adapters/cherrysh \
	-Ithird-party/cherrysh \
	-Ithird-party/cherrysh/cherryrl \
	-Ithird-party/cherryrb

# --- sources ------------------------------------------------------------------

# ThreadX UP kernel: the common/ source set + the cortex_a55 GNU port
# (pinned submodule; provenance and revision in IMPORT-INFO.md). heap.c binds
# tlsf (vendored, see IMPORT-INFO.md) as the system heap; tx_gdb_glue.c is
# the stub's adapter wiring.
KERNEL_SRCS := $(wildcard third-party/threadx/common/src/*.c) \
	port/adapters/threadx/tx_glue.c \
	port/adapters/threadx/tx_up_shim.c \
	port/adapters/threadx/tx_gdb_glue.c \
	port/adapters/threadx/heap.c \
	third-party/tlsf/tlsf.c \
	port/aarch64/gdb/gdb_main.c \
	port/aarch64/gdb/gdb_packet.c \
	port/aarch64/gdb/gdb_arch.c

THREADX_PORT_SRCS := $(wildcard third-party/threadx/ports/cortex_a55/gnu/src/*.S)

# Board bring-up, board-agnostic: MMU/cache/GICv3+ITS/tick, plus the ITS
# diagnostics (the selftest ladder is a boot anchor; itsdump backs the shell
# command and is the MSI substrate evidence for the coming USB line) and the
# SMP parking files (secondaries park on UP; the SMP feat line lights them).
ARCH_SRCS := \
	port/aarch64/mmu.c \
	port/aarch64/memops.c \
	port/aarch64/minilibc.c \
	port/aarch64/cache.c \
	port/aarch64/board_early.c \
	port/aarch64/smp.c \
	port/aarch64/gicv3.c \
	port/aarch64/gicv3_its.c \
	port/aarch64/gicv3_msi.c \
	port/aarch64/its_test.c \
	port/aarch64/itsdump.c \
	port/aarch64/tick.c

ADAPTER_SRCS := \
	port/adapters/cmsis_rtos2_threadx/cmsis_os2_impl.c \
	port/adapters/cherrysh/cherrysh_adapter.c \
	third-party/cherrysh/chry_shell.c \
	third-party/cherrysh/builtin/help.c \
	third-party/cherrysh/builtin/clear.c \
	third-party/cherrysh/builtin/shsize.c \
	third-party/cherrysh/cherryrl/chry_readline.c \
	third-party/cherryrb/chry_ringbuffer.c \

DRIVER_SRCS := drivers/uart_ns16550.c

APP_SRCS := app/main.c app/dbg_scenario.c

# Board assembly is shared; the kernel-side assembly is the seam itself:
# tx_vectors.S (runtime vector table + SPSel entry stubs) plus the kernel
# port's own assembly.
ASM_SRCS := \
	port/aarch64/startup.S \
	port/aarch64/smp_secondary.S \
	port/adapters/threadx/tx_vectors.S \
	$(THREADX_PORT_SRCS)

# --- rules --------------------------------------------------------------------

C_SRCS := $(KERNEL_SRCS) $(ARCH_SRCS) $(ADAPTER_SRCS) $(DRIVER_SRCS) $(APP_SRCS)
OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o)) $(addprefix $(BUILD)/,$(ASM_SRCS:.S=.o))
DEPS := $(OBJS:.o=.d)

# Kernel and adapters see the vendored trees; board, drivers and app do not.
$(BUILD)/third-party/%.o: third-party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/%.o: port/adapters/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

# The rules below deliberately omit INC_ADAPTER.
$(BUILD)/port/aarch64/%.o: port/aarch64/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) -MMD -MP -c $< -o $@

$(BUILD)/port/board/%.o: port/board/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) -MMD -MP -c $< -o $@

$(BUILD)/drivers/%.o: drivers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) -MMD -MP -c $< -o $@

$(BUILD)/app/%.o: app/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) -MMD -MP -c $< -o $@

# The ThreadX port assembly includes tx_port.h / tx_user.h, so these objects
# need the adapter include path; the longer pattern wins the stem match over
# the generic .S rule above.
$(BUILD)/third-party/threadx/%.o: third-party/threadx/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@printf 'image: %s (%s bytes)\n' $@ "$$(stat -c%s $@)"
	@sha256sum $@

# Without this the first explicit target (the ELF rule) would be the default,
# so a bare `make` builds no image at all - which silently leaves a stale
# binary on the TFTP root and makes a failed rebuild look like a boot failure.
.DEFAULT_GOAL := all

.PHONY: all deploy modules gates clean

all: $(TARGET).bin

# Copy to the TFTP root under the name the board's boot profile expects
# (oslab `rtos` profile -> rtos.bin; the banner tells the images apart).
# Records the hash before and after so the transfer is verifiable.
deploy: $(TARGET).bin
	@printf 'before: '; sha256sum /mnt/d/tftpboot/rtos.bin 2>/dev/null || echo '(absent)'
	cp $(TARGET).bin /mnt/d/tftpboot/rtos.bin
	@printf 'after : '; sha256sum /mnt/d/tftpboot/rtos.bin
	@printf 'local : '; sha256sum $(TARGET).bin

# Submodules + patches (policy: IMPORT-INFO.md and patches/README.md).
# Idempotent: a patch that no longer applies cleanly is reported and kept.
#
# third-party/net80211 records a local-path URL (the rk3568_lab NetBSD src
# checkout), so the file:// protocol must be allowed for the whole invocation
# (-c beats any stale local config ordering problem on a fresh clone).
# Its working tree is kept sparse (sys/net80211 + sys/dev/usb + the urtwn
# firmware dist instead of the full ~7 GB src tree); sparse-checkout only
# rewrites the submodule's working tree, the recorded gitlink is untouched.
modules:
	git -c protocol.file.allow=always submodule update --init --recursive
	git -C third-party/net80211 sparse-checkout set \
		sys/net80211 sys/dev/usb external/realtek/urtwn || \
		echo 'note: net80211 sparse-checkout not set (kept full checkout)'
	@for p in patches/*/*.patch; do \
		[ -e "$$p" ] || continue; \
		comp=$$(printf '%s' "$$p" | cut -d/ -f2); \
		if git -C third-party/$$comp apply --check "$$PWD/$$p" 2>/dev/null; then \
			git -C third-party/$$comp apply "$$PWD/$$p" && echo "applied $$p"; \
		else \
			echo "kept    $$p (already applied or inapplicable)"; \
		fi; \
	done

# The merge gates: vendor-trace scan + dependency-direction check.
gates:
	./tools/cleanroom-scan.sh
	./tools/check-deps.sh

clean:
	rm -rf build

-include $(DEPS)
