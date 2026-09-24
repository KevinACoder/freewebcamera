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
# compiler as -D so board_conf.h / board.h derive the secondary-core
# bring-up fan-out and the tick source selection from ONE number.
# Since D42 the mainline image is ThreadX SMP (THREADX defaults to 1);
# FreeRTOS support is SINGLE-CORE only - `make freertos` pins SMP_CORES=1,
# and THREADX=0 with SMP_CORES>1 is a hard error below.
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

# Interface layer: everything reaches CMSIS through here. -Iport/board
# resolves board.h; -Iport/aarch64 is the ARCHITECTURE layer (D46): startup,
# GICv3/ITS, cache/MMU, early print - everything an aarch64 port needs and an
# armv7m (STM32) port would supply as its own parallel directory. -Iport/
# board/$(BOARD) resolves the selected board's board_conf.h (which board.h
# includes). -Idrivers lets the platform glue in port/board/$(BOARD) include
# the IP drivers' internal headers - the board tables feed struct plat data
# INTO the drivers; the reverse direction stays forbidden.
INC_COMMON := -Iinclude -Iport/board -Iport/aarch64 -Iport/board/$(BOARD) -Idrivers
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
	-Iport/adapters/cmsis_rtos2_freertos \
	-Iport/adapters/cherrysh \
	-Ithird-party/cherrysh \
	-Ithird-party/cherrysh/cherryrl \
	-Ithird-party/cherryrb \
	-Iport/adapters/lwip/include \
	-Ithird-party/lwip/src/include \
	-Ithird-party/lwip/apps/ping \
	-Ithird-party/iperf3_embedded \
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
# same shadow-header pattern). The xHCI HCD is this repo's NetBSD port and
# sits in port/adapters/cherryusb/xhci/ - added by the HCD selection block
# below (after both kernel branches, so it applies to either line).
CHERRYUSB_SRCS := \
	third-party/cherryusb/core/usbh_core.c \
	third-party/cherryusb/class/hub/usbh_hub.c \
	third-party/cherryusb/port/ehci/usb_hc_ehci.c \
	third-party/cherryusb/osal/usb_osal_freertos.c

# Board bring-up, board-agnostic part: the same set builds for every board.
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
	port/adapters/freertos/port_glue.c \
	port/adapters/freertos/heap.c \
	port/adapters/cmsis_rtos2_freertos/cmsis_os2_impl.c \
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
	port/adapters/lwip/ping_cmd.c \
	port/adapters/lwip/lwstats_cmd.c \
	third-party/lwip/apps/ping/ping.c \
	port/adapters/lwip/iperf3_port.c \
	port/adapters/lwip/iperf3_cmd.c \
	third-party/iperf3_embedded/iperf3_embedded.c \
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

# (The HCD selection - EHCI-only vs the four-bus multi-HCD image - sits
# after both kernel branches below, so one block serves either line.)

# Console-storm attribution experiment: boot WITHOUT arming the tick. The
# uart driver's pre-arm snapshot then brackets WHEN the INTID 150 line goes
# high relative to the tick: probe-before-kstart clean but arm-time pend=1
# pins the first hit onto the tick arm / first-expiry window. The scheduler
# still starts its first task without a tick; only the first seconds of log
# are meaningful. `make NOTICK=1 SMP_CORES=1 all`.
NOTICK ?= 0
ifeq ($(NOTICK),1)
CFLAGS         += -DEXPERIMENT_NOTICK=1
endif

DRIVER_SRCS := \
	drivers/uart_ns16550.c \
	drivers/dwc_eqos.c \
	drivers/rtl8211f.c \
	drivers/dwc_pcie.c \
	drivers/dwc_msix.c \
	drivers/dwc_ahci.c \
	drivers/dwc_nvme.c \
	drivers/dwc_mmc.c \
	drivers/dwc_mshc.c \
	drivers/rk_i2c.c \
	drivers/rk_tsadc.c \
	drivers/rk_sfc.c

# Platform glue (D45): SoC-bound board data and integration code. The IP
# drivers above are platform-agnostic - they take coordinates and soc hooks
# through plat structs; everything that knows CRU/GRF/iomux/register
# addresses lives here, one directory per board under port/board/.
PLAT_SRCS := \
	port/board/rk3568/rk3568_gmac.c \
	port/board/rk3568/dwc_eqos_rk3568.c \
	port/board/rk3568/rk3568_pcie.c \
	port/board/rk3568/rk3568_sata.c \
	port/board/rk3568/rk3568_sdmmc.c

# Diagnostics/tests are application code (D48): they drive frozen interfaces
# from the app side and never live in drivers/.
APP_SRCS := \
	app/main.c \
	app/smp_test.c \
	app/nvme_diag.c

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

# D42: ThreadX SMP is the MAINLINE kernel - THREADX defaults to 1, so
# `make all` produces the ThreadX SMP image. THREADX=0 selects the
# FreeRTOS port, which is single-core only.
THREADX ?= 1

ifneq ($(THREADX),1)
ifneq ($(SMP_CORES),1)
$(error FreeRTOS is single-core only since D42 - use `make freertos` (THREADX=0 SMP_CORES=1))
endif
endif

# D56: ThreadX single-core comparator. Same app and board layer on the UP
# kernel (common/ + ports/cortex_a55, vendored alongside the SMP set) with
# SMP concurrency removed from the equation; also the carrier for the serial
# gdb stub (port/aarch64/gdb/). Implies SMP_CORES=1 (CNTV tick INTID 27,
# secondaries parked by startup.S). `make threadx-uc`.
THREADX_UP ?= 0

# The ThreadX build (default since D42): same app, same
# board layer, same cherrysh - but the kernel is Eclipse ThreadX
# (common_smp + the cortex_a55_smp GNU port, vendored under
# third-party/threadx/) and the CMSIS-RTOS2 implementation is the
# cmsis_rtos2_threadx twin. Its purpose is to bisect the "4-core shell
# unresponsive" problem: if the identical console/shell path stays live on
# a different SMP kernel, the fault is in the FreeRTOS SMP port, not in the
# board layer.
#
# Own BUILD directory, same reason as ktest: a different kernel's objects
# must never mix into the FreeRTOS image's cache.
#
# Defines, all global (the -DGUEST precedent):
#   -DTHREADX_BUILD=1            image identity: the app banner keys on it
#   -DTX_INCLUDE_USER_DEFINE_FILE  kernel reads the adapter's tx_user.h
#   -DTX_ARMV8_2                 port extracts the core ID from MPIDR Aff1
#                                (RK3568 numbers its cores there, Aff0=0);
#                                see tx_glue.c for the preempt-SGI side of
#                                this story
#   -DEL1                        port targets EL1 (SPSR/ELR selection in
#                                the context-switch assembly)
ifeq ($(THREADX),1)
ifeq ($(THREADX_UP),1)
# UP single-core comparator (D56): the UP kernel source set plus the UP
# cortex_a55 port, and the adapter's DAIF kernel-lock shim (the SMP port's
# spinlock assembly does not exist under common/). -DTX_ARMV8_2 is kept for
# flag parity with the SMP image; no UP source consumes it.
BUILD  := build/$(BOARD)-threadx-uc
TARGET := $(BUILD)/threadx-uc
CFLAGS += -DTHREADX_BUILD=1 -DTHREADX_UP_BUILD=1 -DTX_INCLUDE_USER_DEFINE_FILE -DTX_ARMV8_2 -DEL1
# Debug-carrier build config (D57): symbols plus near-no optimization, so
# GDB's line table places breakpoints on addresses code actually reaches.
# The baseline -O2 stays for the mainline images; UC_OPT=-O0 reproduces the
# reference SDK's CONFIG_DEBUG_NOOPT exact-noopt shape.
UC_OPT ?= -Og
CFLAGS := $(filter-out -O2,$(CFLAGS)) $(UC_OPT) -g3
THREADX_KERNEL_SRCS := $(wildcard third-party/threadx/common/src/*.c)
THREADX_PORT_SRCS := $(wildcard third-party/threadx/ports/cortex_a55/gnu/src/*.S)
KERNEL_SRCS := $(THREADX_KERNEL_SRCS) port/adapters/threadx/tx_glue.c \
	port/adapters/threadx/tx_up_shim.c \
	port/adapters/threadx/tx_gdb_glue.c \
	port/aarch64/gdb/gdb_main.c \
	port/aarch64/gdb/gdb_packet.c \
	port/aarch64/gdb/gdb_arch.c \
	port/adapters/threadx/heap.c third-party/tlsf/tlsf.c
# The carrier's scenario runner: shell commands called from main() (D56).
APP_SRCS += app/dbg_scenario.c
else
BUILD  := build/$(BOARD)-threadx
TARGET := $(BUILD)/threadx-smp
CFLAGS += -DTHREADX_BUILD=1 -DTX_INCLUDE_USER_DEFINE_FILE -DTX_ARMV8_2 -DEL1

# The whole SMP kernel source set (194 files; the UP common/ set is a
# different source selection and is not vendored at all).
THREADX_KERNEL_SRCS := $(wildcard third-party/threadx/common_smp/src/*.c)

# The port assembly, minus tx_thread_smp_core_preempt.S: its ICC_SGI1R
# encoding targets the Aff0 field, which is always 0 on this SoC, so a
# preempt IPI would reach no core and be silently dropped. The adapter
# (port/adapters/threadx/tx_glue.c) provides _tx_thread_smp_core_preempt
# via the board's Aff1-correct SGI sender; registered in IMPORT-INFO.md.
THREADX_PORT_SRCS := $(filter-out \
	third-party/threadx/ports_smp/cortex_a55_smp/gnu/src/tx_thread_smp_core_preempt.S, \
	$(wildcard third-party/threadx/ports_smp/cortex_a55_smp/gnu/src/*.S))
KERNEL_SRCS := $(THREADX_KERNEL_SRCS) port/adapters/threadx/tx_glue.c \
	port/adapters/threadx/heap.c third-party/tlsf/tlsf.c
endif

# The vendored middleware lists. CherryUSB stays stubbed for now (its OSAL
# is the P4 wave); lwIP runs the cmsis sys_arch below - same core list as
# the FreeRTOS branch, contrib's FreeRTOS sys_arch swapped out.
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
	port/adapters/lwip/cmsis/sys_arch.c

FATFS_SRCS := \
	third-party/fatfs/ff.c \
	third-party/fatfs/ffunicode.c

SDMMC_SRCS := \
	third-party/sdmmc/common/fsl_sdmmc_common.c \
	third-party/sdmmc/mmc/fsl_mmc.c \
	third-party/sdmmc/sd/fsl_sd.c \
	third-party/sdmmc/sdio/fsl_sdio.c

# CherryUSB host subset, with the adapted ThreadX osal from the adapter
# (see usb_osal_threadx.c and imports.md).
CHERRYUSB_SRCS := \
	third-party/cherryusb/core/usbh_core.c \
	third-party/cherryusb/class/hub/usbh_hub.c \
	third-party/cherryusb/port/ehci/usb_hc_ehci.c \
	port/adapters/cherryusb/usb_osal_threadx.c

ADAPTER_SRCS := \
	port/adapters/cmsis_rtos2_threadx/cmsis_os2_impl.c \
	port/adapters/cherrysh/cherrysh_adapter.c \
	third-party/cherrysh/chry_shell.c \
	third-party/cherrysh/builtin/help.c \
	third-party/cherrysh/builtin/clear.c \
	third-party/cherrysh/builtin/shsize.c \
	third-party/cherrysh/cherryrl/chry_readline.c \
	third-party/cherryrb/chry_ringbuffer.c \
	port/adapters/periph/periph_cmds.c \
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
	port/adapters/lwip/lwip_diag.c \
	port/adapters/lwip/ethernetif.c \
	port/adapters/lwip/lwip_adapter.c \
	port/adapters/lwip/net_cmds.c \
	port/adapters/lwip/lwstats_cmd.c \
	port/adapters/lwip/ping_cmd.c \
	third-party/lwip/apps/ping/ping.c \
	port/adapters/lwip/iperf3_port.c \
	port/adapters/lwip/iperf3_cmd.c \
	third-party/iperf3_embedded/iperf3_embedded.c \
	port/adapters/cherryusb/usbh_platform.c \
	port/adapters/cherryusb/usbh_adapter.c \
	port/adapters/cherryusb/usbh_cmds.c

# ThreadX's include paths replace the FreeRTOS ones wholesale: the FreeRTOS
# INC_ADAPTER must not leak into this image, or a stray FreeRTOS.h would
# compile against the wrong kernel. The one path kept from the FreeRTOS
# adapter's directory list is cmsis_rtos2_freertos/ itself, and only for the shared,
# kernel-free extension header cmsis_os2_ext.h (osThreadFlagsSetFromISR,
# consumed by the cherrysh adapter and implemented by both CMSIS twins).
ifeq ($(THREADX_UP),1)
KERN_INC := -Ithird-party/threadx/common/inc \
	-Ithird-party/threadx/ports/cortex_a55/gnu/inc
else
KERN_INC := -Ithird-party/threadx/common_smp/inc \
	-Ithird-party/threadx/ports_smp/cortex_a55_smp/gnu/inc
endif
INC_ADAPTER := $(KERN_INC) \
	-Iport/adapters/threadx \
	-Iport/adapters/cmsis_rtos2_threadx \
	-Iport/adapters/cmsis_rtos2_freertos \
	-Ithird-party/tlsf \
	-Iport/adapters/cherrysh \
	-Ithird-party/cherrysh \
	-Ithird-party/cherrysh/cherryrl \
	-Ithird-party/cherryrb \
	-Iport/adapters/lwip/include \
	-Iport/adapters/lwip/cmsis/include \
	-Ithird-party/lwip/src/include \
	-Ithird-party/lwip/apps/ping \
	-Ithird-party/iperf3_embedded \
	-Iport/adapters/cherryusb \
	-Iport/adapters/cherryusb/xhci \
	-Ithird-party/cherryusb/common \
	-Ithird-party/cherryusb/core \
	-Ithird-party/cherryusb/class/hub \
	-Ithird-party/cherryusb/port/ehci \
	-Iport/adapters/fatfs \
	-Ithird-party/fatfs \
	-Iport/adapters/sdmmc/shadow \
	-Iport/adapters/sdmmc \
	-Ithird-party/sdmmc/common \
	-Ithird-party/sdmmc/sd \
	-Ithird-party/sdmmc/osa \
	-Ithird-party/sdmmc/mmc \
	-Ithird-party/sdmmc/sdio \
	-Iport/adapters/stub \
	-Idrivers
endif

# Host-controller selection, shared by both kernel lines (hence placed after
# them - the ThreadX branch reassigns ADAPTER_SRCS wholesale, which is what
# silently swallowed the old XHCI=1 filter; the HCD files belong after it).
#
# Default: the four-bus multi-HCD image. The vendored stack routes per bus
# through a generic HCD ops table (CONFIG_USBHOST_MULTI_HCD, D50): core's
# usb_hc_init/usbh_submit_urb/... dispatch via usbh_hcd_register(), each
# port's entry points are renamed under the same macro (vendored sources
# stay single-contract when the macro is off), and the adapter registers one
# ops table per bus - EHCI0/1 (busid 0/1) plus both DWC3 xHCI instances
# (busid 2/3, the USB3 socket group incl. otg-as-host @ 0xFCC00000).
#
# `make EHCI_ONLY=1`: the legacy one-HCD-per-image shape (each HCD defines
# usb_hc_init / usbh_submit_urb / USBH_IRQHandler directly, no dispatch
# layer) - the M6 mainstream configuration, kept as the regression
# comparison. The old XHCI=1 mutually-exclusive xHCI image is gone: the
# multi-HCD image supersedes it.
EHCI_ONLY ?= 0
ifeq ($(EHCI_ONLY),1)
ADAPTER_SRCS   += port/adapters/cherryusb/usbh_glue.c
else
CFLAGS         += -DCONFIG_USBHOST_MULTI_HCD=1
ADAPTER_SRCS   += port/adapters/cherryusb/usbh_glue.c \
                  port/adapters/cherryusb/xhci/usb_hc_xhci_netbsd.c \
                  port/adapters/cherryusb/xhci/usbh_xhci_glue.c
endif

# --- wlan: vendored net80211 + wpa_supplicant (M7) -------------------------
#
# The net80211 library (third-party/net80211, NetBSD verbatim + the compat
# shadow headers + its own port layer) is bound into the image in four
# compile worlds, mirroring the embox lane's build:
#   BSD world   (-D_KERNEL + the compat/netbsd shadow headers via
#               -idirafter, NO kernel include paths): the verbatim net80211
#               core, the AES module, the urtwn driver.
#   host world  (INC_ADAPTER + the library's port_config.h): the CMSIS-RTOS2
#               OSAL, the firmware resolver, the lwIP netif and the cherryusb
#               glue - the parts the embox lane keeps in its net_bridge.
#   wpa world   (-w + wpa_port_config.h forced): the PSK-only hostap file
#               set and the CMSIS supplicant glue. No BSD headers.
#   wpabsd      (BSD + wpa flags): the two translation units that call
#               net80211 from the supplicant - driver_net80211.c and
#               l2_packet_net80211.c.
# The adapter (port/adapters/net80211/) carries the registry, the worker
# threads, the heap/console bindings and the shell command.
# Claims happen on the CherryUSB hub thread during enumeration, which is
# why app/main.c calls wlan_start() (the OSAL services + firmware registry)
# before usb_start(); the lwIP netif registers lazily after tcpip_init.

NET80211_BSD_SRCS := \
	third-party/net80211/net80211/ieee80211.c \
	third-party/net80211/net80211/ieee80211_amrr.c \
	third-party/net80211/net80211/ieee80211_crypto.c \
	third-party/net80211/net80211/ieee80211_crypto_ccmp.c \
	third-party/net80211/net80211/ieee80211_crypto_none.c \
	third-party/net80211/net80211/ieee80211_input.c \
	third-party/net80211/net80211/ieee80211_netbsd.c \
	third-party/net80211/net80211/ieee80211_node.c \
	third-party/net80211/net80211/ieee80211_output.c \
	third-party/net80211/net80211/ieee80211_proto.c \
	third-party/net80211/crypto/aes/aes_bear.c \
	third-party/net80211/crypto/aes/aes_ccm.c \
	third-party/net80211/crypto/aes/aes_ccm_mbuf.c \
	third-party/net80211/crypto/aes/aes_ct.c \
	third-party/net80211/crypto/aes/aes_ct_dec.c \
	third-party/net80211/crypto/aes/aes_ct_enc.c \
	third-party/net80211/driver/urtwn/urtwn_reg.c \
	third-party/net80211/driver/rtw8189f/rtw8189f_sdio.c \
	third-party/net80211/driver/rtw8189f/rtw8189f_chip.c \
	third-party/net80211/driver/rtw8189f/rtw8189f_reg.c

# rtw88 (RTL8821CU): the imported Linux chip logic + its own Linux-API
# compat layer + the usbdi transport and registration TU. Compiled by the
# dedicated driver/rtw88/% rule below - the dist tree carries generic
# header names (debug.h, main.h, mac.h) whose include paths must not leak
# into the other BSD-world units.
NET80211_RTW_SRCS := \
	third-party/net80211/driver/rtw88/compat/rtw88_compat.c \
	third-party/net80211/driver/rtw88/dist/main.c \
	third-party/net80211/driver/rtw88/dist/util.c \
	third-party/net80211/driver/rtw88/dist/tx.c \
	third-party/net80211/driver/rtw88/dist/rx.c \
	third-party/net80211/driver/rtw88/dist/mac.c \
	third-party/net80211/driver/rtw88/dist/phy.c \
	third-party/net80211/driver/rtw88/dist/coex.c \
	third-party/net80211/driver/rtw88/dist/efuse.c \
	third-party/net80211/driver/rtw88/dist/fw.c \
	third-party/net80211/driver/rtw88/dist/ps.c \
	third-party/net80211/driver/rtw88/dist/sec.c \
	third-party/net80211/driver/rtw88/dist/bf.c \
	third-party/net80211/driver/rtw88/dist/regd.c \
	third-party/net80211/driver/rtw88/dist/sar.c \
	third-party/net80211/driver/rtw88/dist/rtw8821c.c \
	third-party/net80211/driver/rtw88/dist/rtw8821c_table.c \
	third-party/net80211/driver/rtw88/dist/debug.c \
	third-party/net80211/driver/rtw88/rtw88_usb.c \
	third-party/net80211/driver/rtw88/rtw88_chip.c \
	third-party/net80211/driver/rtw88/rtw88u_reg.c

NET80211_HOST_SRCS := \
	third-party/net80211/port/aes_impl_compat.c \
	third-party/net80211/port/osal/embox/port_core.c \
	third-party/net80211/port/net/embox/bsd_mbuf.c \
	third-party/net80211/port/net/embox/bsd_ifnet.c \
	third-party/net80211/port/osal/cmsis_rtos2/osal_cmsis_rtos2.c \
	third-party/net80211/port/osal/cmsis_rtos2/firmware_cmsis.c \
	third-party/net80211/port/net/lwip/lwip_netif.c \
	third-party/net80211/port/bus/usb/cherryusb/usbdi_compat.c \
	third-party/net80211/port/bus/usb/cherryusb/usbh_urtwn_class.c \
	third-party/net80211/port/bus/usb/cherryusb/usbh_modeswitch.c \
	third-party/net80211/port/bus/sd/sdio_compat.c

NET80211_ADAPTER_SRCS := \
	port/adapters/net80211/wlan_adapter.c \
	port/adapters/net80211/wlan_console.c \
	port/adapters/net80211/wlan_cmds.c \
	port/adapters/net80211/wlan_sdio_claim.c \
	port/adapters/net80211/fw_rtl8188eufw.c \
	port/adapters/net80211/fw_rtw8189ffw.c \
	port/adapters/net80211/fw_rtw8821c.c

# The PSK-only file set (no EAP/WPS/P2P/ctrl-iface/SME), the same list the
# embox lane compiles from this fork.
WPA_CORE_SRCS := \
	third-party/wpa_supplicant/src/common/wpa_common.c \
	third-party/wpa_supplicant/src/common/ieee802_11_common.c \
	third-party/wpa_supplicant/src/common/hw_features_common.c \
	third-party/wpa_supplicant/src/drivers/driver_common.c \
	third-party/wpa_supplicant/src/drivers/drivers.c \
	third-party/wpa_supplicant/src/rsn_supp/wpa.c \
	third-party/wpa_supplicant/src/rsn_supp/wpa_ie.c \
	third-party/wpa_supplicant/src/rsn_supp/pmksa_cache.c \
	third-party/wpa_supplicant/src/rsn_supp/preauth.c \
	third-party/wpa_supplicant/src/utils/common.c \
	third-party/wpa_supplicant/src/utils/wpabuf.c \
	third-party/wpa_supplicant/src/utils/base64.c \
	third-party/wpa_supplicant/src/utils/bitfield.c \
	third-party/wpa_supplicant/src/utils/wpa_debug.c \
	third-party/wpa_supplicant/src/crypto/crypto_internal.c \
	third-party/wpa_supplicant/src/crypto/aes-internal.c \
	third-party/wpa_supplicant/src/crypto/aes-internal-dec.c \
	third-party/wpa_supplicant/src/crypto/aes-internal-enc.c \
	third-party/wpa_supplicant/src/crypto/aes-wrap.c \
	third-party/wpa_supplicant/src/crypto/aes-unwrap.c \
	third-party/wpa_supplicant/src/crypto/aes-omac1.c \
	third-party/wpa_supplicant/src/crypto/sha1.c \
	third-party/wpa_supplicant/src/crypto/sha1-internal.c \
	third-party/wpa_supplicant/src/crypto/sha1-prf.c \
	third-party/wpa_supplicant/src/crypto/sha1-pbkdf2.c \
	third-party/wpa_supplicant/src/crypto/md5.c \
	third-party/wpa_supplicant/src/crypto/md5-internal.c \
	third-party/wpa_supplicant/src/crypto/rc4.c \
	third-party/wpa_supplicant/src/crypto/sha256.c \
	third-party/wpa_supplicant/src/crypto/sha256-internal.c \
	third-party/wpa_supplicant/src/crypto/sha256-prf.c \
	third-party/wpa_supplicant/src/crypto/tls_none.c \
	third-party/wpa_supplicant/wpa_supplicant/wpa_supplicant.c \
	third-party/wpa_supplicant/wpa_supplicant/events.c \
	third-party/wpa_supplicant/wpa_supplicant/scan.c \
	third-party/wpa_supplicant/wpa_supplicant/bss.c \
	third-party/wpa_supplicant/wpa_supplicant/config.c \
	third-party/wpa_supplicant/wpa_supplicant/config_none.c \
	third-party/wpa_supplicant/wpa_supplicant/notify.c \
	third-party/wpa_supplicant/wpa_supplicant/wpas_glue.c \
	third-party/wpa_supplicant/wpa_supplicant/bssid_ignore.c \
	third-party/wpa_supplicant/wpa_supplicant/eap_register.c \
	third-party/wpa_supplicant/wpa_supplicant/op_classes.c \
	third-party/wpa_supplicant/wpa_supplicant/rrm.c \
	third-party/wpa_supplicant/wpa_supplicant/robust_av.c

WPA_PORT_SRCS := \
	port/adapters/wpa_supplicant/os_port.c \
	port/adapters/wpa_supplicant/eloop_port.c \
	port/adapters/wpa_supplicant/supp_main_cmsis.c \
	port/adapters/wpa_supplicant/wpa_cmd.c \
	port/adapters/wpa_supplicant/l2_packet_net80211.c \
	port/adapters/wpa_supplicant/driver_net80211.c

ADAPTER_SRCS += $(NET80211_ADAPTER_SRCS)

# D56, UP carrier only: shell-free debug image. cherrysh goes (its
# interrupt-driven RX path is exactly what the gdb stub must not fight
# over the one UART), and every *_cmds.c goes with it - they call
# csh_printf(), which lives in chry_shell.c. The command surface is
# operator UI, not boot state; the carrier drives the same workers
# through app/dbg_scenario.c and gdb function calls instead.
ifeq ($(THREADX_UP),1)
SHELL_SRCS := \
	port/adapters/cherrysh/cherrysh_adapter.c \
	third-party/cherrysh/chry_shell.c \
	third-party/cherrysh/builtin/help.c \
	third-party/cherrysh/builtin/clear.c \
	third-party/cherrysh/builtin/shsize.c \
	third-party/cherrysh/cherryrl/chry_readline.c \
	third-party/cherryrb/chry_ringbuffer.c \
	port/adapters/lwip/net_cmds.c \
	port/adapters/lwip/lwstats_cmd.c \
	port/adapters/lwip/ping_cmd.c \
	port/adapters/lwip/iperf3_cmd.c \
	port/adapters/fatfs/fatfs_cmds.c \
	port/adapters/sdmmc/sdmmc_cmds.c \
	port/adapters/periph/periph_cmds.c \
	port/adapters/cherryusb/usbh_cmds.c \
	port/adapters/net80211/wlan_cmds.c \
	port/adapters/wpa_supplicant/wpa_cmd.c
ADAPTER_SRCS := $(filter-out $(SHELL_SRCS),$(ADAPTER_SRCS))
WPA_PORT_SRCS := $(filter-out port/adapters/wpa_supplicant/wpa_cmd.c,$(WPA_PORT_SRCS))
# scenario 7 (wl-load): drives wpa/lwIP/iperf3 directly and parks in the
# stub on a load stall - needs the stub, so UP builds only.
ADAPTER_SRCS += port/adapters/net80211/wl_load_scenario.c
endif

INC_ADAPTER += -Ithird-party/net80211 \
	-Ithird-party/net80211/port/osal/cmsis_rtos2/compat \
	-Ithird-party/net80211/port/net/lwip \
	-Iport/adapters/net80211 \
	-Iport/adapters/wpa_supplicant \
	-Iport/adapters/wpa_supplicant/shim \
	-Ithird-party/wpa_supplicant/src \
	-Ithird-party/wpa_supplicant/src/utils \
	-Ithird-party/wpa_supplicant/src/drivers \
	-Ithird-party/wpa_supplicant/src/l2_packet \
	-Ithird-party/wpa_supplicant/wpa_supplicant

# BSD world: the compat shadow headers sit AFTER the system directories
# (-idirafter) so libc headers keep winning where both exist.
NET80211_BSD_INC := -Ithird-party/net80211 \
	-Ithird-party/net80211/port/osal/cmsis_rtos2/compat \
	-idirafter third-party/net80211/compat/netbsd
NET80211_BSD_CFG := -D_KERNEL -D_COMPAT_SYS_SYSCTL_H_ -include stdarg.h \
	-include third-party/net80211/port/port_config_bsd.h
# rtw88: BSD-world config plus the driver's debug switch; the compat
# shadow (linux/*.h, net/mac80211.h) sits ahead of the NetBSD one and the
# dist tree resolves its bare-name headers from its own directory.
NET80211_RTW_INC := $(NET80211_BSD_INC) \
	-Ithird-party/net80211/driver/rtw88 \
	-Ithird-party/net80211/driver/rtw88/compat \
	-Ithird-party/net80211/driver/rtw88/dist
NET80211_RTW_CFG := $(NET80211_BSD_CFG) -DCONFIG_RTW88_DEBUG -std=gnu11
NET80211_HOST_CFG := -D_KERNEL -include stdarg.h \
	-include third-party/net80211/port/port_config.h
# shim/ first: minimal net/if.h + netinet/in.h for the wpa world (newlib
# has none and the compat shadow would drag the BSD malloc macros in).
WPA_INC := -Iport/adapters/wpa_supplicant/shim \
	-Ithird-party/wpa_supplicant \
	-Ithird-party/wpa_supplicant/src \
	-Ithird-party/wpa_supplicant/src/utils \
	-Ithird-party/wpa_supplicant/src/drivers \
	-Ithird-party/wpa_supplicant/src/l2_packet \
	-Ithird-party/wpa_supplicant/wpa_supplicant \
	-Iport/adapters/wpa_supplicant
WPA_CFG := -w -include stdarg.h -include port/adapters/wpa_supplicant/wpa_port_config.h

# The core count, the HCD selection and the kernel shape all change codegen
# everywhere but leave no trace make's timestamp logic can see: a
# SMP_CORES=2 build after a =4 build (or EHCI_ONLY=1 after the default, or
# THREADX_UP after the SMP kernel) would silently relink stale objects and
# link two HCDs into one image. Stamp all three into the build directory
# (per image variant - BUILD above already separates them) and drop the
# directory entirely on mismatch. Sits after the KTEST block so BUILD_STAMP
# follows the same BUILD the rules use.
BUILD_STAMP := $(BUILD)/.smp-cores
ifeq ($(shell cat $(BUILD_STAMP) 2>/dev/null),$(SMP_CORES) $(EHCI_ONLY) $(THREADX) $(THREADX_UP))
else
$(shell rm -rf $(BUILD))
endif
$(BUILD_STAMP):
	@mkdir -p $(BUILD) && echo '$(SMP_CORES) $(EHCI_ONLY) $(THREADX) $(THREADX_UP)' > $(BUILD_STAMP)

# Board assembly is shared; the kernel-side assembly is the seam itself and
# therefore per-kernel: the FreeRTOS image links portasm_smp.S (context
# switch, IRQ entry, yield) + port_vectors.S, the ThreadX image links
# tx_vectors.S (its runtime vector table + SPSel entry stubs) plus the
# port's own assembly, minus the preempt-SGI file the adapter replaces.
ifeq ($(THREADX),1)
ASM_SRCS := \
	port/aarch64/startup.S \
	port/aarch64/smp_secondary.S \
	port/adapters/threadx/tx_vectors.S \
	$(THREADX_PORT_SRCS)
else
ASM_SRCS := \
	port/aarch64/startup.S \
	port/aarch64/smp_secondary.S \
	port/adapters/freertos/portasm.S \
	port/adapters/freertos/port_vectors.S
endif

# --- rules ----------------------------------------------------------------

	C_SRCS := $(KERNEL_SRCS) $(LWIP_SRCS) $(FATFS_SRCS) $(SDMMC_SRCS) $(CHERRYUSB_SRCS) $(ARCH_SRCS) $(ADAPTER_SRCS) $(DRIVER_SRCS) $(PLAT_SRCS) $(APP_SRCS) \
		$(NET80211_BSD_SRCS) $(NET80211_RTW_SRCS) $(NET80211_HOST_SRCS) $(WPA_CORE_SRCS) $(WPA_PORT_SRCS)
OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o)) $(addprefix $(BUILD)/,$(ASM_SRCS:.S=.o))
DEPS := $(OBJS:.o=.d)

# Kernel and adapters see the vendored trees; board, drivers and app do not.
$(BUILD)/third-party/%.o: third-party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

# The net80211/wpa worlds get their own rules: the stem-specific patterns
# win make's shortest-stem match over the generic third-party/%.o rule.
# BSD world (verbatim NetBSD): -w silences the vanilla tree's own warnings;
# no kernel include paths reach it.
$(BUILD)/third-party/net80211/%.o: third-party/net80211/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(INC_COMMON) $(NET80211_BSD_INC) $(NET80211_BSD_CFG) -MMD -MP -c $< -o $@

# rtw88 group: the deepest stem wins over the net80211/% rule above, so
# only these units see the driver's compat/dist include paths (their
# generic header names must not shadow anything else).
$(BUILD)/third-party/net80211/driver/rtw88/%.o: third-party/net80211/driver/rtw88/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(INC_COMMON) $(NET80211_RTW_INC) $(NET80211_RTW_CFG) -MMD -MP -c $< -o $@

# The library's host-world units: CMSIS OSAL, lwIP netif, cherryusb glue.
$(BUILD)/third-party/net80211/port/%.o: third-party/net80211/port/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(NET80211_BSD_INC) $(NET80211_HOST_CFG) -MMD -MP -c $< -o $@

# The hostap tree: PSK-only set, wpa_port_config.h forced everywhere.
$(BUILD)/third-party/wpa_supplicant/%.o: third-party/wpa_supplicant/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

# The supplicant glue: wpa world flags over the adapter include set.
$(BUILD)/port/adapters/wpa_supplicant/%.o: port/adapters/wpa_supplicant/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

# The two both-worlds units: they call net80211 directly (BSD world) and
# link into the supplicant (wpa world) - exactly the embox lane's split.
$(BUILD)/port/adapters/wpa_supplicant/driver_net80211.o: port/adapters/wpa_supplicant/driver_net80211.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(NET80211_BSD_INC) $(NET80211_BSD_CFG) $(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/wpa_supplicant/l2_packet_net80211.o: port/adapters/wpa_supplicant/l2_packet_net80211.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(NET80211_BSD_INC) $(NET80211_BSD_CFG) $(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/%.o: port/adapters/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

# These three rules deliberately omit INC_ADAPTER.
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

# The ThreadX port assembly includes tx_user.h (-DTX_INCLUDE_USER_DEFINE_FILE),
# so these objects need the adapter include path - unlike the generic .S rule
# above, which keeps board assembly on the kernel-free paths. The longer
# pattern wins make's shortest-stem match over $(BUILD)/%.o.
$(BUILD)/third-party/threadx/%.o: third-party/threadx/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

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

.PHONY: all clean deploy gates ktest ktest-deploy threadx threadx-deploy threadx-uc threadx-uc-deploy freertos freertos-deploy

all: $(TARGET).bin

# Kernel-test image: same tree, test-suite entry, own build directory and
# config values. Deployed under the same TFTP name (freertos.bin) because
# that is what the boot profile loads; the banner line on the console
# ("freewebcamera ktest") tells the images apart.
ktest:
	@$(MAKE) THREADX=0 SMP_CORES=1 KTEST=1 all

ktest-deploy:
	@$(MAKE) THREADX=0 SMP_CORES=1 KTEST=1 deploy

# ThreadX SMP is the MAINLINE image (D42): `make all` builds it; these
# targets remain as explicit aliases (same TFTP deploy name, the banner
# tells the kernels apart - "freewebcamera M0 - RK3568 ThreadX SMP carrier").
threadx:
	@$(MAKE) THREADX=1 all

threadx-deploy:
	@$(MAKE) THREADX=1 deploy

# ThreadX UP single-core comparator + gdb stub carrier (D56).
threadx-uc:
	@$(MAKE) THREADX=1 THREADX_UP=1 SMP_CORES=1 all

threadx-uc-deploy:
	@$(MAKE) THREADX=1 THREADX_UP=1 SMP_CORES=1 deploy

# FreeRTOS single-core support/comparator image (D42).
freertos:
	@$(MAKE) THREADX=0 SMP_CORES=1 all

freertos-deploy:
	@$(MAKE) THREADX=0 SMP_CORES=1 deploy

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
	port/adapters/stub/wlan_stub.c \
	port/adapters/stub/fs_stub.c \
	port/adapters/stub/sdio_stub.c \
	port/adapters/stub/smp_stub.c \
	port/adapters/stub/usb_stub.c \
	port/aarch64/gicv3.c \
	port/aarch64/gicv3_its.c \
	port/aarch64/gicv3_msi.c \
	port/aarch64/its_test.c \
	port/aarch64/itsdump.c \
	port/aarch64/board_early.c \
	port/aarch64/memops.c \
	port/aarch64/minilibc.c \
	port/aarch64/cache.c \
	port/aarch64/mmu.c \
	port/aarch64/smp.c

STUB_OBJS := $(addprefix $(BUILD)/stub/,$(STUB_SRCS:.c=.o)) \
	     $(addprefix $(BUILD)/stub/,$(DRIVER_SRCS:.c=.o)) \
	     $(addprefix $(BUILD)/stub/,$(PLAT_SRCS:.c=.o)) \
	     $(addprefix $(BUILD)/stub/,$(APP_SRCS:.c=.o))

$(BUILD)/stub/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Iport/board -Iport/aarch64 \
		-Iport/board/$(BOARD) -Iport/adapters/stub -Idrivers \
		-MMD -MP -c $< -o $@

.PHONY: k4
k4: $(BUILD)/freertos-stub.elf

# Only startup.S is included: it is the board entry point every build needs.
# vectors.S is deliberately NOT here - it branches to the FreeRTOS port's
# handlers, so it belongs to the kernel side of the seam, not to the layers
# this target is testing. What is under test is that app/ and drivers/ link
# with no kernel; the kernel's own glue is allowed to be absent.
STUB_ASM := $(BUILD)/stub/port/aarch64/startup.o

$(BUILD)/freertos-stub.elf: $(STUB_OBJS) $(STUB_ASM)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(STUB_OBJS) $(STUB_ASM) \
		-nostdlib -static -T port/board/$(BOARD)/$(BOARD).ld \
		-Wl,--build-id=none -Wl,--no-warn-rwx-segments -o $@
	@printf 'K4 OK: app/ and drivers/ link with no kernel and no shell\n'
	@printf '        (CMSIS-RTOS2 and include/shell.h are the only interfaces they see)\n'

$(BUILD)/stub/port/aarch64/startup.o: port/aarch64/startup.S
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
	rm -rf build/$(BOARD) build/$(BOARD)-ktest build/$(BOARD)-threadx

-include $(DEPS)
