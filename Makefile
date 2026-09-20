# Build for the FreeRTOS carrier image.
#
# Produces a flat raw binary. The RK3568 board loads it with
#   tftp 0xa000000 freertos.bin ; go 0xa000000
# (bootelf is unusable on that board - it faults at the jump). The
# aarch64-virt board runs under qemu-system-aarch64 (see the run-virt
# target).
#
# Layers, and the include-path rule that enforces them:
#   include/      interface layer: vendored CMSIS headers + our gap headers
#   port/board/   CPU bring-up (startup, MMU, GICv3, tick);
#                 common/ is board-agnostic, <board>/ holds the conf + link
#                 script, selected by BOARD=
#   port/adapters/ one directory per vendored component
#   drivers/      CMSIS driver implementations
#   app/          application
#   third-party/  vendored upstream, byte-identical
#
# app/ and drivers/ never get third-party/ or kernel include paths: if they
# need something it has to come through include/. tools/check-deps.sh checks
# the same rule statically.

# Board selection: the port/board/<board>/ directory supplies board_conf.h
# (register bases, interrupt numbers, MMU windows, PSCI conventions) and the
# link script. Default unchanged since forever: the RK3568 board.
BOARD ?= rk3568

# Number of cores this image runs on (1..4 on RK3568). Forwarded to the
# compiler as -D so board_conf.h / board.h and FreeRTOSConfig.h derive the
# kernel core count, the port arrays, the app fan-out and the tick source
# selection from ONE number. `make SMP_CORES=1` is the single-core
# comparator image. An earlier layout had the flag in FreeRTOSConfig.h but
# never passed it through, so it silently built 4-core every time.
SMP_CORES ?= 4

# Bare-metal toolchain. Set explicitly rather than inherited: non-interactive
# shells do not source ~/.bashrc, and silently picking up a Linux-targeted
# compiler produces a link against glibc assumptions that cannot work here.
AARCH64_CROSS_PATH ?= /home/zhugy/workspace/env/src/xpack-aarch64-none-elf-gcc-13.2.1-1.1
CROSS_COMPILE ?= $(AARCH64_CROSS_PATH)/bin/aarch64-none-elf-
CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size

BUILD   := build/$(BOARD)
TARGET  := $(BUILD)/freertos

# -mgeneral-regs-only: the port saves no FP state, so any FP instruction is a
#   bug we want the compiler to prevent rather than discover as corruption.
# -DGUEST: selects the port's EL1 path. Without it the port targets EL3 and
#   asserts on CurrentEL at scheduler start.
# -mno-outline-atomics: the MCS kernel locks use __atomic exchange/CAS; with
#   outline atomics GCC emits calls to __aarch64_* helpers that a -nostdlib
#   image has no library for. Inlined LDXR/STXR loops need no library.
CFLAGS := -O2 -g -std=c11 -Wall -Wextra \
	-ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-march=armv8-a -mgeneral-regs-only -mstrict-align -mno-outline-atomics \
	-DGUEST -DSMP_CORES=$(SMP_CORES) \
	-Wno-unused-parameter -Wno-sign-compare

# Recursively expanded so the -Map name follows TARGET when the KTEST=1
# block below re-points it at the ktest image.
LDFLAGS = -nostdlib -static -T port/board/$(BOARD)/$(BOARD).ld \
	-Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map=$(TARGET).map

# --- include paths --------------------------------------------------------

# Interface layer: everything reaches CMSIS through here. -Iport/board and
# -Iport/board/common resolve board.h and the board-agnostic bring-up
# headers (gicv3_its.h); -Iport/board/$(BOARD) resolves the selected board's
# board_conf.h (which board.h includes).
INC_COMMON := -Iinclude -Iport/board -Iport/board/common -Iport/board/$(BOARD)
# Available only to adapters (they are the only layer allowed to touch
# vendored code and kernel internals). The port lives in the adapter too:
# portmacro.h resolves from -Iport/adapters/freertos, and the vendored
# single-core portable/GCC/ARM_AARCH64_SRE directory is deliberately NOT on
# the include path - the SMP port replaced it, and an accidental include of
# the single-core portmacro.h would silently compile the wrong kernel.
INC_ADAPTER := -Ithird-party/FreeRTOS-Kernel/include \
	-Iport/adapters/freertos \
	-Iport/adapters/freertos/tests \
	-Iport/adapters/freertos/tests/include \
	-Iport/adapters/cmsis_rtos2 \
	-Iport/adapters/cherrysh \
	-Ithird-party/cherrysh \
	-Ithird-party/cherrysh/cherryrl \
	-Ithird-party/cherryrb \
	-Iport/adapters/lwip/include \
	-Ithird-party/lwip/src/include \
	-Ithird-party/lwip/contrib/ports/freertos/include \
	-Iport/adapters/fatfs \
	-Ithird-party/fatfs \
	-Iport/adapters/sdmmc/shadow \
	-Iport/adapters/sdmmc \
	-Iport/adapters/cherryusb \
	-Iport/adapters/cherryusb/xhci \
	-Ithird-party/cherryusb/common \
	-Ithird-party/cherryusb/core \
	-Ithird-party/cherryusb/class/hub \
	-Ithird-party/cherryusb/port/ehci \
	-Idrivers \
	-Ithird-party/sdmmc/common \
	-Ithird-party/sdmmc/sd \
	-Ithird-party/sdmmc/osa \
	-Ithird-party/sdmmc/mmc \
	-Ithird-party/sdmmc/sdio

# --- sources --------------------------------------------------------------

KERNEL_SRCS := \
	third-party/FreeRTOS-Kernel/tasks.c \
	third-party/FreeRTOS-Kernel/queue.c \
	third-party/FreeRTOS-Kernel/list.c \
	third-party/FreeRTOS-Kernel/timers.c \
	third-party/FreeRTOS-Kernel/event_groups.c \
	third-party/FreeRTOS-Kernel/portable/MemMang/heap_4.c \
	port/adapters/freertos/port.c

# lwIP 2.2.1, vendored. The set follows upstream src/Filelists.mk for the core,
# IPv4 and sequential-API groups, plus the FreeRTOS sys_arch from contrib.
# Files whose feature is switched off in lwipopts.h (sockets, DNS, autoip)
# compile to nothing, which upstream's own makefiles rely on too. IPv6, PPP,
# bridged/lowpan netifs and the apps directories are not vendored at all.
LWIP_SRCS := \
	third-party/lwip/src/core/def.c \
	third-party/lwip/src/core/inet_chksum.c \
	third-party/lwip/src/core/init.c \
	third-party/lwip/src/core/ip.c \
	third-party/lwip/src/core/mem.c \
	third-party/lwip/src/core/memp.c \
	third-party/lwip/src/core/netif.c \
	third-party/lwip/src/core/pbuf.c \
	third-party/lwip/src/core/raw.c \
	third-party/lwip/src/core/stats.c \
	third-party/lwip/src/core/sys.c \
	third-party/lwip/src/core/tcp.c \
	third-party/lwip/src/core/tcp_in.c \
	third-party/lwip/src/core/tcp_out.c \
	third-party/lwip/src/core/timeouts.c \
	third-party/lwip/src/core/udp.c \
	third-party/lwip/src/core/ipv4/acd.c \
	third-party/lwip/src/core/ipv4/dhcp.c \
	third-party/lwip/src/core/ipv4/etharp.c \
	third-party/lwip/src/core/ipv4/icmp.c \
	third-party/lwip/src/core/ipv4/ip4.c \
	third-party/lwip/src/core/ipv4/ip4_addr.c \
	third-party/lwip/src/core/ipv4/ip4_frag.c \
	third-party/lwip/src/api/api_lib.c \
	third-party/lwip/src/api/api_msg.c \
	third-party/lwip/src/api/err.c \
	third-party/lwip/src/api/netbuf.c \
	third-party/lwip/src/api/netdb.c \
	third-party/lwip/src/api/netifapi.c \
	third-party/lwip/src/api/sockets.c \
	third-party/lwip/src/api/tcpip.c \
	third-party/lwip/src/netif/ethernet.c \
	third-party/lwip/contrib/ports/freertos/sys_arch.c

# FatFs R0.16 (ChaN), vendored from the lab's ff16.zip. Only the module and its
# Unicode tables are vendored: ffconf.h comes from the adapter (upstream's own
# copy would shadow it, since ff.c includes "ffconf.h" by relative path first),
# and diskio.c / ffsystem.c are integrator-supplied sample code that the
# adapter replaces with its own.
FATFS_SRCS := \
	third-party/fatfs/ff.c \
	third-party/fatfs/ffunicode.c

# NXP fsl_sdmmc protocol layer, vendored (BSD-3). Only the card protocol and
# its generic OSA are vendored: no SD card module (the sdmmc0 slot carries an
# SDIO card, not storage), no host/ reference implementations (they are for
# NXP controllers), no examples/templates. The SDK headers the stack expects
# (fsl_common.h, fsl_os_abstraction.h) and the host contract
# (fsl_sdmmc_host.h) come from the adapter's shadow directory, which the
# include order places ahead of the vendored tree.
SDMMC_SRCS := \
	third-party/sdmmc/common/fsl_sdmmc_common.c \
	third-party/sdmmc/mmc/fsl_mmc.c \
	third-party/sdmmc/sd/fsl_sd.c \
	third-party/sdmmc/sdio/fsl_sdio.c

# CherryUSB (upstream master 0e40349b), vendored. Host-only clean subset:
# the usbh core, the hub class (both panel roots carry an onboard CH334P),
# the EHCI HCD for the two usb2host controllers and the FreeRTOS osal. The
# device-side cores, other classes and the vendor xHCI ports are not
# vendored; usb_config.h comes from the adapter (see the sdmmc note above -
# same shadow-header pattern).
CHERRYUSB_SRCS := \
	third-party/cherryusb/core/usbh_core.c \
	third-party/cherryusb/class/hub/usbh_hub.c \
	third-party/cherryusb/port/ehci/usb_hc_ehci.c \
	third-party/cherryusb/osal/usb_osal_freertos.c

# Board bring-up, board-agnostic part: the same set builds for every board.
BOARD_SRCS := \
	port/board/common/mmu.c \
	port/board/common/memops.c \
	port/board/common/minilibc.c \
	port/board/common/cache.c \
	port/board/common/board_early.c \
	port/board/common/smp.c \
	port/board/common/gicv3.c \
	port/board/common/gicv3_its.c \
	port/board/common/gicv3_msi.c \
	port/board/common/its_test.c \
	port/board/common/itsdump.c \
	port/board/common/tick.c

ADAPTER_SRCS := \
	port/adapters/freertos/port_glue.c \
	port/adapters/freertos/heap.c \
	port/adapters/cmsis_rtos2/cmsis_os2_impl.c \
	port/adapters/cherrysh/cherrysh_adapter.c \
	third-party/cherrysh/chry_shell.c \
	third-party/cherrysh/builtin/help.c \
	third-party/cherrysh/builtin/clear.c \
	third-party/cherrysh/builtin/shsize.c \
	third-party/cherrysh/cherryrl/chry_readline.c \
	third-party/cherryrb/chry_ringbuffer.c \
	port/adapters/lwip/lwip_diag.c \
	port/adapters/lwip/ethernetif.c \
	port/adapters/lwip/lwip_adapter.c \
	port/adapters/lwip/net_cmds.c \
	port/adapters/fatfs/blkdev.c \
	port/adapters/fatfs/diskio.c \
	port/adapters/fatfs/fatfs_os.c \
	port/adapters/fatfs/fatfs_adapter.c \
	port/adapters/fatfs/fatfs_cmds.c \
	port/adapters/sdmmc/sdmmc_osa.c \
	port/adapters/sdmmc/sdmmc_glue_irq.c \
	port/adapters/sdmmc/sdmmc_dispatch.c \
	port/adapters/sdmmc/sdmmc_host_dwmmc.c \
	port/adapters/sdmmc/sdmmc_host_dwmshc.c \
	port/adapters/sdmmc/sdmmc_storage.c \
	port/adapters/sdmmc/sdmmc_adapter.c \
	port/adapters/sdmmc/sdmmc_cmds.c \
	port/adapters/periph/periph_cmds.c \
	port/adapters/cherryusb/usbh_platform.c \
	port/adapters/cherryusb/usbh_adapter.c \
	port/adapters/cherryusb/usbh_cmds.c

# Host-controller selection. The vendored stack is one-HCD-per-image (each
# HCD defines usb_hc_init / usbh_submit_urb / USBH_IRQHandler), so EHCI and
# xHCI builds are mutually exclusive until a dispatch layer exists. Default
# stays EHCI (the M6 mainstream image); `make XHCI=1` builds the DWC3/xHCI
# enumeration image for the USB3 socket group instead.
XHCI ?= 0
ifeq ($(XHCI),1)
CFLAGS         += -DUSBH_HCD_XHCI=1
CHERRYUSB_SRCS := $(filter-out third-party/cherryusb/port/ehci/usb_hc_ehci.c,$(CHERRYUSB_SRCS))
ADAPTER_SRCS   += port/adapters/cherryusb/xhci/usb_hc_xhci_netbsd.c \
                  port/adapters/cherryusb/xhci/usbh_xhci_glue.c
else
ADAPTER_SRCS   += port/adapters/cherryusb/usbh_glue.c
endif

DRIVER_SRCS := \
	drivers/uart_ns16550.c \
	drivers/dwc_eqos.c \
	drivers/dwc_eqos_rk3568.c \
	drivers/rtl8211f.c \
	drivers/rk3568_gmac.c \
	drivers/dwc_pcie.c \
	drivers/rk3568_pcie.c \
	drivers/dwc_msix.c \
	drivers/dwc_ahci.c \
	drivers/rk3568_sata.c \
	drivers/dwc_nvme.c \
	drivers/dwc_mmc.c \
	drivers/dwc_mshc.c \
	drivers/rk3568_sdmmc.c \
	drivers/rk_i2c.c \
	drivers/rk_tsadc.c \
	drivers/rk_sfc.c

APP_SRCS := \
	app/main.c \
	app/smp_test.c

# Kernel test suite (official FreeRTOS TestRunner + Common/Minimal files,
# see port/adapters/freertos/tests/ and IMPORT-INFO.md). Only the ktest
# image links these; the main image neither compiles nor links them.
TEST_SRCS := \
	port/adapters/freertos/tests/test_runner.c \
	port/adapters/freertos/tests/intqueue_timer.c \
	port/adapters/freertos/tests/ktest_support.c \
	port/adapters/freertos/tests/minimal/BlockQ.c \
	port/adapters/freertos/tests/minimal/GenQTest.c \
	port/adapters/freertos/tests/minimal/PollQ.c \
	port/adapters/freertos/tests/minimal/QPeek.c \
	port/adapters/freertos/tests/minimal/QueueOverwrite.c \
	port/adapters/freertos/tests/minimal/QueueSet.c \
	port/adapters/freertos/tests/minimal/QueueSetPolling.c \
	port/adapters/freertos/tests/minimal/AbortDelay.c \
	port/adapters/freertos/tests/minimal/blocktim.c \
	port/adapters/freertos/tests/minimal/countsem.c \
	port/adapters/freertos/tests/minimal/death.c \
	port/adapters/freertos/tests/minimal/dynamic.c \
	port/adapters/freertos/tests/minimal/integer.c \
	port/adapters/freertos/tests/minimal/recmutex.c \
	port/adapters/freertos/tests/minimal/semtest.c \
	port/adapters/freertos/tests/minimal/EventGroupsDemo.c \
	port/adapters/freertos/tests/minimal/IntQueue.c \
	port/adapters/freertos/tests/minimal/IntSemTest.c \
	port/adapters/freertos/tests/minimal/TaskNotify.c \
	port/adapters/freertos/tests/minimal/TaskNotifyArray.c \
	port/adapters/freertos/tests/minimal/TimerDemo.c \
	port/adapters/freertos/tests/minimal/StaticAllocation.c

# KTEST=1 swaps the app entry and pulls the test suite in. BUILD gets its
# own directory: the ktest config values (heap, priorities, tick hook)
# differ, so sharing object files with the main image would mix two
# different kernels' worth of compiled config into one .o cache.
ifeq ($(KTEST),1)
BUILD        := build/$(BOARD)-ktest
TARGET       := $(BUILD)/freertos-ktest
CFLAGS       += -DKTEST_BUILD=1
APP_SRCS     := app/ktest_main.c app/smp_test.c
ADAPTER_SRCS += $(TEST_SRCS)
# The AbortDelay test exercises xTaskAbortDelay against a stream buffer, so
# the ktest image needs the stream buffer API compiled in (the main image
# does not use it; see configUSE_STREAM_BUFFERS in FreeRTOSConfig.h).
KERNEL_SRCS  += third-party/FreeRTOS-Kernel/stream_buffer.c
endif

# The core count and the HCD selection both change codegen everywhere but
# leave no trace make's timestamp logic can see: a SMP_CORES=2 build after a
# =4 build (or XHCI=1 after the default) would silently relink stale objects
# and link two HCDs into one image. Stamp both values into the build
# directory (per image variant - BUILD above already separates them) and
# drop the directory entirely on mismatch. Sits after the KTEST block so
# BUILD_STAMP follows the same BUILD the rules use.
BUILD_STAMP := $(BUILD)/.smp-cores
ifeq ($(shell cat $(BUILD_STAMP) 2>/dev/null),$(SMP_CORES) $(XHCI))
else
$(shell rm -rf $(BUILD))
endif
$(BUILD_STAMP):
	@mkdir -p $(BUILD) && echo '$(SMP_CORES) $(XHCI)' > $(BUILD_STAMP)

ASM_SRCS := \
	port/board/common/startup.S \
	port/board/common/smp_secondary.S \
	port/adapters/freertos/portasm_smp.S \
	port/adapters/freertos/port_vectors.S

# --- rules ----------------------------------------------------------------

C_SRCS := $(KERNEL_SRCS) $(LWIP_SRCS) $(FATFS_SRCS) $(SDMMC_SRCS) $(CHERRYUSB_SRCS) $(BOARD_SRCS) $(ADAPTER_SRCS) $(DRIVER_SRCS) $(APP_SRCS)
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

$(TARGET).elf: $(OBJS) $(BUILD_STAMP)
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

.PHONY: all clean deploy gates ktest ktest-deploy

all: $(TARGET).bin

# Kernel-test image: same tree, test-suite entry, own build directory and
# config values. Deployed under the same TFTP name (freertos.bin) because
# that is what the boot profile loads; the banner line on the console
# ("freewebcamera ktest") tells the images apart.
ktest:
	@$(MAKE) KTEST=1 all

ktest-deploy:
	@$(MAKE) KTEST=1 deploy

# Copy to the TFTP root under the name the freertos boot profile expects.
# Records the hash before and after so the transfer is verifiable.
deploy: $(TARGET).bin
	@printf 'before: '; sha256sum /mnt/d/tftpboot/freertos.bin 2>/dev/null || echo '(absent)'
	cp $(TARGET).bin /mnt/d/tftpboot/freertos.bin
	@printf 'after : '; sha256sum /mnt/d/tftpboot/freertos.bin
	@printf 'local : '; sha256sum $(TARGET).bin

# --- kernel-replacement stub build (K4) ------------------------------------
#
# M0 claims CMSIS-RTOS2 is a real boundary, not a label. The way to test that
# claim is to delete the kernel and see whether the layers above still build:
# this links app/ and drivers/ against a null CMSIS-RTOS2 implementation, with
# no FreeRTOS sources, no kernel include paths and no port glue.
#
# It does not run and is never deployed - linking at all is the result. If a
# file in app/ or drivers/ ever reaches for a kernel symbol, this target stops
# linking while the normal build keeps working.
STUB_SRCS := \
	port/adapters/stub/cmsis_os2_stub.c \
	port/adapters/stub/shell_stub.c \
	port/adapters/stub/net_stub.c \
	port/adapters/stub/fs_stub.c \
	port/adapters/stub/sdio_stub.c \
	port/adapters/stub/smp_stub.c \
	port/adapters/stub/usb_stub.c \
	port/board/common/gicv3.c \
	port/board/common/gicv3_its.c \
	port/board/common/gicv3_msi.c \
	port/board/common/its_test.c \
	port/board/common/itsdump.c \
	port/board/common/board_early.c \
	port/board/common/memops.c \
	port/board/common/minilibc.c \
	port/board/common/cache.c \
	port/board/common/mmu.c \
	port/board/common/smp.c

STUB_OBJS := $(addprefix $(BUILD)/stub/,$(STUB_SRCS:.c=.o)) \
	     $(addprefix $(BUILD)/stub/,$(DRIVER_SRCS:.c=.o)) \
	     $(addprefix $(BUILD)/stub/,$(APP_SRCS:.c=.o))

$(BUILD)/stub/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Iport/board -Iport/board/common \
		-Iport/board/$(BOARD) -Iport/adapters/stub \
		-MMD -MP -c $< -o $@

.PHONY: k4
k4: $(BUILD)/freertos-stub.elf

# Only startup.S is included: it is the board entry point every build needs.
# vectors.S is deliberately NOT here - it branches to the FreeRTOS port's
# handlers, so it belongs to the kernel side of the seam, not to the layers
# this target is testing. What is under test is that app/ and drivers/ link
# with no kernel; the kernel's own glue is allowed to be absent.
STUB_ASM := $(BUILD)/stub/port/board/common/startup.o

$(BUILD)/freertos-stub.elf: $(STUB_OBJS) $(STUB_ASM)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(STUB_OBJS) $(STUB_ASM) \
		-nostdlib -static -T port/board/$(BOARD)/$(BOARD).ld \
		-Wl,--build-id=none -Wl,--no-warn-rwx-segments -o $@
	@printf 'K4 OK: app/ and drivers/ link with no kernel and no shell\n'
	@printf '        (CMSIS-RTOS2 and include/shell.h are the only interfaces they see)\n'

$(BUILD)/stub/port/board/common/startup.o: port/board/common/startup.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Iport/board -Iport/board/$(BOARD) -c $< -o $@

# The gates are the things M0 is allowed to be judged on. k4 is included
# because "the kernel interface is real" is a claim this build has to keep
# making, not a one-off check: if app/ or drivers/ ever picks up a kernel
# dependency, this fails here rather than at some later kernel swap.
gates: k4
	./tools/cleanroom-scan.sh
	./tools/check-deps.sh

clean:
	rm -rf build/$(BOARD) build/$(BOARD)-ktest

-include $(DEPS)
