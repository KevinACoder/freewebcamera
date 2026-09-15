# Build for the RK3568 FreeRTOS carrier.
#
# Produces a flat raw binary the board loads with
#   tftp 0xa000000 freertos.bin ; go 0xa000000
# (bootelf is unusable on this board - it faults at the jump).
#
# Layers, and the include-path rule that enforces them:
#   include/      interface layer: vendored CMSIS headers + our gap headers
#   port/board/   CPU bring-up (startup, MMU, GICv3, tick)
#   port/adapters/ one directory per vendored component
#   drivers/      CMSIS driver implementations
#   app/          application
#   third-party/  vendored upstream, byte-identical
#
# app/ and drivers/ never get third-party/ or kernel include paths: if they
# need something it has to come through include/. tools/check-deps.sh checks
# the same rule statically.

# Bare-metal toolchain. Set explicitly rather than inherited: non-interactive
# shells do not source ~/.bashrc, and silently picking up a Linux-targeted
# compiler produces a link against glibc assumptions that cannot work here.
AARCH64_CROSS_PATH ?= /home/zhugy/workspace/env/src/xpack-aarch64-none-elf-gcc-13.2.1-1.1
CROSS_COMPILE ?= $(AARCH64_CROSS_PATH)/bin/aarch64-none-elf-
CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size

BUILD   := build
TARGET  := $(BUILD)/freertos

# -mgeneral-regs-only: the port saves no FP state, so any FP instruction is a
#   bug we want the compiler to prevent rather than discover as corruption.
# -DGUEST: selects the port's EL1 path. Without it the port targets EL3 and
#   asserts on CurrentEL at scheduler start.
CFLAGS := -O2 -g -std=c11 -Wall -Wextra \
	-ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-march=armv8-a -mgeneral-regs-only -mstrict-align \
	-DGUEST \
	-Wno-unused-parameter -Wno-sign-compare

LDFLAGS := -nostdlib -static -T port/board/rk3568.ld \
	-Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map=$(TARGET).map

# --- include paths --------------------------------------------------------

# Interface layer: everything reaches CMSIS through here.
INC_COMMON := -Iinclude -Iport/board
# Available only to adapters (they are the only layer allowed to touch
# vendored code and kernel internals).
INC_ADAPTER := -Ithird-party/FreeRTOS-Kernel/include \
	-Ithird-party/FreeRTOS-Kernel/portable/GCC/ARM_AARCH64_SRE \
	-Iport/adapters/freertos

# --- sources --------------------------------------------------------------

KERNEL_SRCS := \
	third-party/FreeRTOS-Kernel/tasks.c \
	third-party/FreeRTOS-Kernel/queue.c \
	third-party/FreeRTOS-Kernel/list.c \
	third-party/FreeRTOS-Kernel/timers.c \
	third-party/FreeRTOS-Kernel/event_groups.c \
	third-party/FreeRTOS-Kernel/portable/MemMang/heap_4.c \
	third-party/FreeRTOS-Kernel/portable/GCC/ARM_AARCH64_SRE/port.c

BOARD_SRCS := \
	port/board/mmu.c \
	port/board/memops.c \
	port/board/cache.c \
	port/board/board_early.c \
	port/board/gicv3.c \
	port/board/tick.c

ADAPTER_SRCS := \
	port/adapters/freertos/port_glue.c \
	port/adapters/freertos/heap.c \
	port/adapters/cmsis_rtos2/cmsis_os2_impl.c

DRIVER_SRCS := \
	drivers/uart_ns16550.c

APP_SRCS := \
	app/main.c

ASM_SRCS := \
	port/board/startup.S \
	port/board/vectors.S \
	third-party/FreeRTOS-Kernel/portable/GCC/ARM_AARCH64_SRE/portASM.S

# --- rules ----------------------------------------------------------------

C_SRCS := $(KERNEL_SRCS) $(BOARD_SRCS) $(ADAPTER_SRCS) $(DRIVER_SRCS) $(APP_SRCS)
OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o)) $(addprefix $(BUILD)/,$(ASM_SRCS:.S=.o))
DEPS := $(OBJS:.o=.d)

# Kernel and adapters see the vendored trees; board, drivers and app do not.
$(BUILD)/third-party/%.o: third-party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/%.o: port/adapters/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

# These three rules deliberately omit INC_ADAPTER.
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

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@printf 'image: %s (%s bytes)\n' $@ "$$(stat -c%s $@)"
	@sha256sum $@

# Without this the first explicit target (the ELF rule) is the default, so a
# bare `make` builds no image at all - which silently leaves a stale
# freertos.bin on the TFTP root and makes a failed rebuild look like a boot
# failure on the board.
.DEFAULT_GOAL := all

.PHONY: all clean deploy gates

all: $(TARGET).bin

# Copy to the TFTP root under the name the freertos boot profile expects.
# Records the hash before and after so the transfer is verifiable.
deploy: $(TARGET).bin
	@printf 'before: '; sha256sum /mnt/d/tftpboot/freertos.bin 2>/dev/null || echo '(absent)'
	cp $(TARGET).bin /mnt/d/tftpboot/freertos.bin
	@printf 'after : '; sha256sum /mnt/d/tftpboot/freertos.bin
	@printf 'local : '; sha256sum $(TARGET).bin

gates:
	./tools/cleanroom-scan.sh
	./tools/check-deps.sh

clean:
	rm -rf $(BUILD)

-include $(DEPS)
