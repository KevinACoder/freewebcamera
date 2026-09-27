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

# =============================================================================
# Build configuration (configs/)
# =============================================================================
# The image is described by a config file, configs/<CONFIG>.conf (default
# full).  Every config includes configs/base.conf - the key table, with the
# defaults and the documentation - and adds configs/fragments/* for the
# capabilities it wants, so a new configuration is a short list of includes
# rather than a copy of the defaults:
#
#   make                 build CONFIG=full (all three wireless lines + camera)
#   make CONFIG=min      kernel + shell only (the trim proof)
#   make configs         list the available configurations
#   make show-config     the resolved key table + this image's artifact paths
#
# Keys are make variables named CONFIG_*; the same names become macros in the
# generated <build>/config.h, which is the only place C code reads them.  The
# pre-config command-line switches keep working as aliases (WLAN_NIC=iwm|
# urtwn|rtw8189f|all, UVC=, UAC=, UVC_DEBUG=, UC_OPT=) - see the alias block.
#
# Two different switches, two different costs:
#   * CONFIG=<other>    picks another build directory; no clean needed.
#   * CONFIG_<KEY>=<v>  changes a value inside one config.  The -D defines and
#                       vendored-world flags are invisible to the dependency
#                       files, so the stamp below refuses to build over
#                       objects from a different key set: run `make
#                       clean-config` (or `make clean`) first.
CONFIG_DIR := configs
CONFIG ?= full
CONFIG_NAME ?= $(CONFIG)

CONFIG_FILE := $(CONFIG_DIR)/$(CONFIG).conf
ifeq ($(wildcard $(CONFIG_FILE)),)
$(error unknown CONFIG=$(CONFIG); available: $(patsubst $(CONFIG_DIR)/%.conf,%,$(wildcard $(CONFIG_DIR)/*.conf)))
endif
include $(CONFIG_FILE)

# The table: every key the build understands.  check-config fails on a key
# that appears in a config file but not here (a typo, usually) and on drift
# between this list and the config.h.in template.
CONFIG_KEYS := CONFIG_NAME CONFIG_BOARD CONFIG_SMP_CORES CONFIG_OPT \
	CONFIG_SHELL CONFIG_NET CONFIG_UVC CONFIG_UAC \
	CONFIG_BUS_USB CONFIG_BUS_PCIE CONFIG_BUS_SDIO CONFIG_USB_BACKEND \
	CONFIG_USB_BACKEND_CHERRYUSB CONFIG_USBHOST_XHCI \
	CONFIG_NIC_IWM CONFIG_NIC_URTWN CONFIG_NIC_RTW8189F \
	CONFIG_BSD_DIAGNOSTIC CONFIG_IWM_DEBUG \
	CONFIG_USB_DEBUG_DEFAULT CONFIG_EHCI_DEBUG_DEFAULT CONFIG_XHCI_DEBUG_DEFAULT \
	CONFIG_USBHIST_SIZE CONFIG_UVC_DEBUG \
	CONFIG_HEAP_BASE CONFIG_HEAP_BYTES \
	CONFIG_TX_TIMER_STACK CONFIG_LWIP_THREAD_STACK CONFIG_WLAN_WORKER_STACK \
	CONFIG_WPA_THREAD_STACK CONFIG_SHELL_STACK CONFIG_APP_START_STACK \
	CONFIG_LWIP_MEM_SIZE CONFIG_LWIP_PBUF_POOL_SIZE CONFIG_LWIP_PBUF_BUFSIZE \
	CONFIG_AV_DUMP_RING_BYTES CONFIG_BASE_LOADED

# Keys that become macros in the generated header.  CONFIG_OPT is a build flag
# rather than an image property, and CONFIG_BASE_LOADED is an internal guard
# for the include chain - neither belongs in the header.
CONFIG_HDR_KEYS := $(filter-out CONFIG_OPT CONFIG_BASE_LOADED,$(CONFIG_KEYS))

# --- legacy aliases ----------------------------------------------------------
# The pre-config switches translate into keys.  Passing the old switch and the
# new key at the same time is ambiguous, so it is rejected rather than
# silently resolved one way; a value the old switch never accepted is too.
ifeq ($(origin WLAN_NIC),command line)
ifneq ($(filter command line,$(foreach k,CONFIG_NIC_IWM CONFIG_NIC_URTWN CONFIG_NIC_RTW8189F,$(origin $(k)))),)
$(error WLAN_NIC= and CONFIG_NIC_*= are both set; pass one or the other)
endif
ifeq ($(filter $(WLAN_NIC),iwm urtwn rtw8189f all),)
$(error WLAN_NIC must be one of: iwm, urtwn, rtw8189f, all)
endif
CONFIG_NIC_IWM := $(if $(filter $(WLAN_NIC),iwm all),1,0)
CONFIG_NIC_URTWN := $(if $(filter $(WLAN_NIC),urtwn all),1,0)
CONFIG_NIC_RTW8189F := $(if $(filter $(WLAN_NIC),rtw8189f all),1,0)
endif

ifeq ($(origin UVC),command line)
ifneq ($(origin CONFIG_UVC),command line)
ifeq ($(filter $(UVC),0 1),)
$(error UVC must be 0 or 1)
endif
CONFIG_UVC := $(UVC)
endif
endif

ifeq ($(origin UAC),command line)
ifneq ($(origin CONFIG_UAC),command line)
ifeq ($(filter $(UAC),0 1),)
$(error UAC must be 0 or 1)
endif
CONFIG_UAC := $(UAC)
endif
endif

ifeq ($(origin UVC_DEBUG),command line)
ifneq ($(origin CONFIG_UVC_DEBUG),command line)
ifeq ($(filter $(UVC_DEBUG),0 1),)
$(error UVC_DEBUG must be 0 or 1)
endif
CONFIG_UVC_DEBUG := $(UVC_DEBUG)
endif
endif

ifeq ($(origin UC_OPT),command line)
ifneq ($(origin CONFIG_OPT),command line)
CONFIG_OPT := $(UC_OPT)
endif
endif

# --- implications ------------------------------------------------------------
# A wireless line selects the bus it sits on and the network world it runs in;
# the camera and the microphone are USB devices; the network world needs a
# shell to be driven from.  Setting a bus key by hand can add a bus (a future
# USB-only image) but cannot remove one a line needs.
ifeq ($(CONFIG_NIC_IWM),1)
CONFIG_BUS_PCIE := 1
endif
ifeq ($(CONFIG_NIC_URTWN),1)
CONFIG_BUS_USB := 1
endif
ifeq ($(CONFIG_NIC_RTW8189F),1)
CONFIG_BUS_SDIO := 1
endif
ifneq ($(filter 1,$(CONFIG_NIC_IWM) $(CONFIG_NIC_URTWN) $(CONFIG_NIC_RTW8189F) $(CONFIG_UVC) $(CONFIG_UAC)),)
CONFIG_NET := 1
endif
ifneq ($(filter 1,$(CONFIG_UVC) $(CONFIG_UAC)),)
CONFIG_BUS_USB := 1
endif
# UAC rides on the UVC block: the audio shell commands (`uvc audio ...`) and
# the raw-stream dump channel both live there today, so CONFIG_UAC=1 with
# CONFIG_UVC=0 would not link.  configs/fragments/uac.conf selects UVC for
# this reason; an explicit UVC=0 against a UAC=1 is a contradiction the user
# should resolve, not something to silently override (auto-enabling the camera
# block would build video code nobody asked for).  Splitting uvc_cmds.c into a
# video and an audio command TU is a registered follow-up that retires this.
ifeq ($(CONFIG_UAC),1)
ifeq ($(CONFIG_UVC),0)
$(error CONFIG_UAC=1 needs the UVC block (the `uvc audio ...` commands and the dump channel live in it): pass CONFIG_UVC=1 as well, or set CONFIG_UAC=0. A config that includes $(CONFIG_DIR)/fragments/uac.conf gets UVC automatically)
endif
endif
ifeq ($(CONFIG_NET),1)
CONFIG_SHELL := 1
endif

# --- USB host stack choice (D-C1③) -------------------------------------------
# One backend per image: the two stacks bring their own controllers up and both
# define a Driver_USB_HOST_* object, so which one is linked is a build key, not
# a runtime lookup.  CONFIG_USB_BACKEND is the human-facing name; the derived
# 0/1 switch is what C code and the source lists below key on.  This sits after
# the implications above because CONFIG_BUS_USB may itself have been derived
# from a NIC key.
ifeq ($(filter $(CONFIG_USB_BACKEND),netbsd cherryusb),)
$(error CONFIG_USB_BACKEND must be netbsd or cherryusb, got '$(CONFIG_USB_BACKEND)')
endif
CONFIG_USB_BACKEND_CHERRYUSB := $(if $(filter cherryusb,$(CONFIG_USB_BACKEND)),1,0)
ifeq ($(CONFIG_USB_BACKEND_CHERRYUSB),1)
ifeq ($(CONFIG_BUS_USB),0)
$(error CONFIG_USB_BACKEND=cherryusb needs the USB bus: select the urtwn line (CONFIG_NIC_URTWN=1) or CONFIG_BUS_USB=1 as well)
endif
# Upstream CherryUSB has no isochronous support (the iso entry points are
# declared and never defined; iso is the commercial edition's feature), so the
# camera and the microphone cannot ride this backend.  A hard error beats an
# image that links uvideo against a stack with no iso engine (R1 in
# issues/20260928-feat-cherryusb_risk.md).
ifneq ($(filter 1,$(CONFIG_UVC) $(CONFIG_UAC)),)
$(error CONFIG_USB_BACKEND=cherryusb cannot carry UVC/UAC: upstream CherryUSB has no isochronous transfers. Use the netbsd backend for the camera/microphone lines)
endif
endif

# --- validation --------------------------------------------------------------
# A config file that forgets its includes would silently build the minimal
# image; base.conf sets this marker as its last line.
ifneq ($(CONFIG_BASE_LOADED),1)
$(error $(CONFIG_FILE) does not include $(CONFIG_DIR)/base.conf (the key defaults and documentation))
endif
# The secondaries park in the UP image; the SMP line is a separate branch.
# Better a hard error here than a half-SMP image that mis-programs the GIC.
ifneq ($(CONFIG_SMP_CORES),1)
$(error CONFIG_SMP_CORES=$(CONFIG_SMP_CORES) is not supported on this trunk (UP image only; the SMP line is feat/threadx-smp). See AGENTS.md)
endif

# Board selection: port/board/<board>/ supplies board_conf.h (register bases,
# interrupt numbers, MMU windows) and the link script.
BOARD := $(CONFIG_BOARD)

# Bare-metal toolchain, set explicitly (non-interactive shells do not source
# ~/.bashrc - silently picking up a Linux-targeted compiler links against
# glibc assumptions that cannot work here).
AARCH64_CROSS_PATH ?= $(HOME)/workspace/env/src/xpack-aarch64-none-elf-gcc-13.2.1-1.1
CROSS_COMPILE ?= $(AARCH64_CROSS_PATH)/bin/aarch64-none-elf-
CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE    := $(CROSS_COMPILE)size

# One build directory per configuration: switching CONFIG= never reuses the
# previous image's objects, so no clean is needed between configs.
BUILD   := build/$(BOARD)-threadx-uc-$(CONFIG_NAME)
TARGET  := $(BUILD)/threadx-uc
# The generated header every world includes, and the stamp that ties the tree
# to one resolved key set.
CONFIG_HDR   := $(BUILD)/config.h
CONFIG_STAMP := $(BUILD)/.config-stamp

# -mgeneral-regs-only: the port saves no FP state, so any FP instruction is a
#   bug we want the compiler to prevent rather than discover as corruption.
# -DGUEST/-DEL1: the port runs at EL1 (SPSR/ELR selection in the context-switch
#   assembly; CurrentEL asserts in the board layer).
# -mno-outline-atomics: a -nostdlib image has no library for __aarch64_*
#   helpers; inlined LDXR/STXR loops need none.
# -DTX_INCLUDE_USER_DEFINE_FILE: the kernel reads the adapter's tx_user.h.
# The feature switches are NOT here any more: they live in the generated
# config.h (see configs/).  Only the flags the compiler itself needs, plus the
# SMP core count that startup assembly reads, stay on the command line.
CFLAGS := $(CONFIG_OPT) -g3 -std=c11 -Wall -Wextra \
	-ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
	-march=armv8-a -mgeneral-regs-only -mstrict-align -mno-outline-atomics \
	-DGUEST -DEL1 -DSMP_CORES=$(CONFIG_SMP_CORES) \
	-DTHREADX_BUILD=1 -DTHREADX_UP_BUILD=1 \
	-DTX_INCLUDE_USER_DEFINE_FILE

LDFLAGS = -nostdlib -static -T port/board/$(BOARD)/$(BOARD).ld \
	-Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-Map=$(TARGET).map

# --- include paths -----------------------------------------------------------

# $(BUILD) first: the generated config.h lives there and every project TU that
# reads a key includes it by name.  The vendored trees have no config.h on
# their include paths (verified for libbsd, lwip, netutils, sdmmc; wpa's own
# config.h sits in wpa_supplicant/ and src/utils/ and is only ever reached by
# quote-include from those directories), so the extra -I cannot shadow
# anything upstream compiles against.
INC_COMMON := -I$(BUILD) -Iinclude -Iport/board -Iport/aarch64 -Iport/board/$(BOARD) -Idrivers

# Adapters only: they are the only layer allowed to touch vendored code and
# kernel internals. cmsis_os2_ext.h (osThreadFlagsSetFromISR) lives with the
# ThreadX CMSIS twin - it is the only kernel on this trunk.
INC_ADAPTER := -Ithird-party/threadx/common/inc \
	-Ithird-party/threadx/ports/cortex_a55/gnu/inc \
	-Iport/adapters/threadx \
	-Iport/adapters/cmsis_rtos2_threadx \
	-Ithird-party/tlsf \
	-Ithird-party/printf \
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
	third-party/printf/printf.c \
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
	port/aarch64/tick.c \
	port/aarch64/reset.c

# Shell adapter + cherrysh itself.  SHELL=0 drops the shell and every command
# TU with it (commands register through the FSymTab section, so a command is
# compiled in iff its translation unit is).
ADAPTER_SRCS := port/adapters/cmsis_rtos2_threadx/cmsis_os2_impl.c

ifeq ($(CONFIG_SHELL),1)
ADAPTER_SRCS += \
	port/adapters/cherrysh/cherrysh_adapter.c \
	third-party/cherrysh/chry_shell.c \
	third-party/cherrysh/builtin/help.c \
	third-party/cherrysh/builtin/clear.c \
	third-party/cherrysh/builtin/shsize.c \
	third-party/cherrysh/cherryrl/chry_readline.c \
	third-party/cherryrb/chry_ringbuffer.c
endif

# netutils (ping/tftp/iperf3/ntp/telnet) is part of the network world.
ifeq ($(CONFIG_NET),1)
ADAPTER_SRCS += \
	port/adapters/netutils/netutils_shim.c \
	port/adapters/netutils/tftp_port.c \
	port/adapters/netutils/iperf3_port.c \
	port/adapters/netutils/iperf3_cmd.c \
	port/adapters/netutils/ntp_port.c \
	port/adapters/netutils/telnet_port.c
endif

DRIVER_SRCS := drivers/uart_ns16550.c

# Board data, per line (D45 shape: drivers/ keeps the platform-agnostic IP,
# the coordinates live with the board).
BOARD_SRCS :=
BOARD_PCIE_SRCS := port/board/$(BOARD)/rk3568_pcie.c

# Bus lines: the driver + board data each bus brings up.  The bus keys are set
# by the wireless lines (and by UVC/UAC for USB) through the implications
# above; gating the sources on them keeps a bus out of an image that has no
# line needing it (the SDIO host + fsl_sdmmc stack are a sizeable slab).
ifeq ($(CONFIG_BUS_PCIE),1)
DRIVER_SRCS  += drivers/dwc_pcie.c drivers/pci_msix.c
BOARD_SRCS   += $(BOARD_PCIE_SRCS)
endif
ifeq ($(CONFIG_BUS_SDIO),1)
DRIVER_SRCS  += drivers/dwc_mmc.c
BOARD_SRCS   += port/board/$(BOARD)/rk3568_sdmmc.c
endif

# Command TUs register through the shell's command table; without a shell they
# would drag the shell headers in for nothing.  (The sdmmc command TU rides
# with the rest of the SDIO adapter units in SDMMC_ADAPTER_SRCS below.)
ifeq ($(CONFIG_SHELL),1)
ifeq ($(CONFIG_BUS_PCIE),1)
ADAPTER_SRCS += port/adapters/pcie/pcie_cmds.c
endif
endif

APP_SRCS := app/main.c app/dbg_scenario.c

# --- net80211 + usb world (feat/net80211) -------------------------------------
# The NetBSD import (third-party/libbsd, see IMPORT-INFO.md) compiles in
# its own world: the compat shadow headers stand in for the NetBSD kernel
# headers (compat first, so the shadows win over anything the sparse tree
# carries), port_config_bsd.h force-includes the preamble, and the if_urtwn
# driver compiles inside port/adapters/libbsd/urtwn_reg.c so its static
# CFATTACH glue stays intact. The adapter impl units (bus_dma/autoconf/osal
# backends) compile in the same world; the shell-facing adapter files compile
# like any other adapter code.
#
# CONFIG_NET=0 drops this world entirely: net80211, the BSD port core, every
# bus line, the camera and the microphone.  The per-line and per-bus groups
# below stay named so a future finer split (lwIP alone, wpa alone) has the
# lists ready, but only CONFIG_NET=1 accumulates them into the build.

LIBBSD_NET80211_SRCS := \
	third-party/libbsd/sys/net80211/ieee80211.c \
	third-party/libbsd/sys/net80211/ieee80211_amrr.c \
	third-party/libbsd/sys/net80211/ieee80211_crypto.c \
	third-party/libbsd/sys/net80211/ieee80211_crypto_ccmp.c \
	third-party/libbsd/sys/net80211/ieee80211_crypto_none.c \
	third-party/libbsd/sys/net80211/ieee80211_input.c \
	third-party/libbsd/sys/net80211/ieee80211_netbsd.c \
	third-party/libbsd/sys/net80211/ieee80211_node.c \
	third-party/libbsd/sys/net80211/ieee80211_output.c \
	third-party/libbsd/sys/net80211/ieee80211_proto.c \
	third-party/libbsd/sys/crypto/aes/aes_bear.c \
	third-party/libbsd/sys/crypto/aes/aes_ccm.c \
	third-party/libbsd/sys/crypto/aes/aes_ccm_mbuf.c \
	third-party/libbsd/sys/crypto/aes/aes_ct.c \
	third-party/libbsd/sys/crypto/aes/aes_ct_dec.c \
	third-party/libbsd/sys/crypto/aes/aes_ct_enc.c

# The USB host stack (usbdi + hub + ehci/xhci): the urtwn line's bus, and the
# bus the camera and the microphone sit on.
LIBBSD_USB_SRCS := \
	third-party/libbsd/sys/dev/usb/usbdi.c \
	third-party/libbsd/sys/dev/usb/usbdi_util.c \
	third-party/libbsd/sys/dev/usb/usb_mem.c \
	third-party/libbsd/sys/dev/usb/usb_subr.c \
	third-party/libbsd/sys/dev/usb/usb.c \
	third-party/libbsd/sys/dev/usb/usb_quirks.c \
	third-party/libbsd/sys/dev/usb/uhub.c \
	third-party/libbsd/sys/dev/usb/usbroothub.c \
	third-party/libbsd/sys/dev/usb/ehci.c \
	third-party/libbsd/sys/dev/usb/xhci.c

LIBBSD_IMPL_CORE_SRCS := \
	port/adapters/libbsd/aes_impl_compat.c \
	port/adapters/libbsd/bsd_bus.c \
	port/adapters/libbsd/bsd_autoconf.c \
	port/adapters/libbsd/bsd_file.c \
	port/adapters/libbsd/bsd_kernhist.c \
	port/adapters/libbsd/bsd_subr_prf.c \
	port/adapters/libbsd/osal/osal_cmsis_rtos2.c \
	port/adapters/libbsd/osal/firmware_cmsis.c \
	port/adapters/libbsd/net/bsd_mbuf.c \
	port/adapters/libbsd/net/bsd_ifnet.c

LIBBSD_ADAPTER_CORE_SRCS := \
	port/adapters/libbsd/wlan_adapter.c \
	port/adapters/libbsd/wlan_console.c \
	port/adapters/libbsd/wlan_cmds.c

# One line, one driver TU set: the bus glue + platform + firmware blob of
# each wireless line (the driver .c compiles inside the *_reg.c wrapper so
# its static CFATTACH glue stays intact).
#
# The USB platform bring-up is bus-level, not urtwn-level: the EHCI/xHCI
# host composition is what the camera enumerates behind, so an iwm+UVC
# image (no USB wireless driver at all) still needs it.  Only the RTL
# firmware blob belongs to the urtwn line itself.
LIBBSD_USB_PLATFORM_SRCS := \
	port/adapters/libbsd/usb_platform.c \
	port/adapters/libbsd/usb_xhci_platform.c

# The USB request abstraction (feat/usbport + feat/cherryusb_ehci):
# include/usb_host.h is the interface, the backend is the only file that talks
# to its own stack, and the command TU is the in-tree consumer.  Which backend
# is linked is the CONFIG_USB_BACKEND key (D-C1③, one per image): the NetBSD
# backend compiles in the BSD world (its own rule below), the CherryUSB backend
# in the CherryUSB world (rules further down).  The command TU is ordinary
# adapter code in both.
ifeq ($(CONFIG_USB_BACKEND_CHERRYUSB),1)
USB_HOST_SRCS := \
	port/adapters/usb/usb_host_cherryusb.c
else
USB_HOST_SRCS := \
	port/adapters/usb/usb_host_netbsd.c
endif

USB_HOST_CMD_SRCS := \
	port/adapters/usb/usb_host_cmds.c

# The backend-neutral half of the USB platform bring-up (PD_PIPE + PHY
# reference clocks + VBUS + the usb2phy1 domain).  It talks to the CRU/PMU/GRF
# only - no host stack headers - so both backends link this one copy.
USB_DOMAIN_SRCS := \
	port/adapters/usb/usb_domain.c

# --- the CherryUSB host stack (feat/cherryusb_ehci) ---------------------------
# The vendored upstream tree compiled as-is (core + the hub class + the EHCI
# HCD + the OSAL), plus the adapter: the shadow usb_config.h, the low-level
# glue (IRQ/cache/console), the platform start path and the shell command.
# The tree itself is never edited - local differences go through patches/.
# The xHCI port (feat/cherryusb_xhci) is our own code landing through
# patches/cherryusb/ as upstream-tree files (no open upstream xHCI port
# exists - port/xhci/ carries only excluded vendor blobs); it joins the
# image on CONFIG_USBHOST_XHCI.
CHERRYUSB_SUB_SRCS := \
	third-party/cherryusb/core/usbh_core.c \
	third-party/cherryusb/class/hub/usbh_hub.c \
	third-party/cherryusb/port/ehci/usb_hc_ehci.c

ifeq ($(CONFIG_USBHOST_XHCI),1)
CHERRYUSB_SUB_SRCS += \
	third-party/cherryusb/port/xhci/usb_hc_xhci.c
endif

CHERRYUSB_ADAPTER_SRCS := \
	port/adapters/cherryusb/usb_osal_threadx.c \
	port/adapters/cherryusb/usbh_glue.c \
	port/adapters/cherryusb/usbh_platform.c

CHERRYUSB_CMD_SRCS := \
	port/adapters/cherryusb/usbh_cmds.c

# The shadow usb_config.h first: every vendored unit includes it by name, and
# this path must win over anything else on the include list (cherrysh ships a
# usb_config.h of its own).  usb_board.h lives with the NetBSD adapter and is
# the single copy of the board constants this file also needs.
CHERRYUSB_INC := -Iport/adapters/cherryusb -Iport/adapters/libbsd \
	-Iport/adapters/usb \
	-Ithird-party/cherryusb/core \
	-Ithird-party/cherryusb/common \
	-Ithird-party/cherryusb/class/hub \
	-Ithird-party/cherryusb/port/ehci \
	-Ithird-party/cherryusb/port/xhci

LIBBSD_URTWN_ADAPTER_SRCS := \
	port/adapters/libbsd/fw_rtl8188eufw.c

LIBBSD_PCIE_ADAPTER_SRCS := \
	port/adapters/libbsd/fw_iwlwifi7260.c \
	port/adapters/libbsd/iwm_reg.c \
	port/adapters/libbsd/pcie_glue.c

# The SDIO line (feat/net80211_sdio): rtw8189f over the fsl_sdmmc stack.
# The driver's transport/chip TUs compile in the frozen-import BSD world;
# the in-file-compile wrapper (rtw8189f_reg.c) and the sdmmc(9) shim
# (sd/sdio_compat.c) join the adapter impl units; the claim layer (the
# fsl_sdio ops binding + the explicit probe) and the firmware array join
# the shell-facing adapter files.
LIBBSD_SDIO_BSD_SRCS := \
	third-party/libbsd/sys/dev/sdmmc/rtw8189f_sdio.c \
	third-party/libbsd/sys/dev/sdmmc/rtw8189f_chip.c

LIBBSD_SDIO_IMPL_SRCS := \
	port/adapters/libbsd/rtw8189f_reg.c \
	port/adapters/libbsd/sd/sdio_compat.c

LIBBSD_SDIO_ADAPTER_SRCS := \
	port/adapters/libbsd/fw_rtw8189ffw.c \
	port/adapters/libbsd/wlan_sdio_claim.c

LIBBSD_BSD_SRCS :=
LIBBSD_IMPL_SRCS :=
LIBBSD_ADAPTER_SRCS :=

ifeq ($(CONFIG_NET),1)
LIBBSD_BSD_SRCS     := $(LIBBSD_NET80211_SRCS)
LIBBSD_IMPL_SRCS    := $(LIBBSD_IMPL_CORE_SRCS)
LIBBSD_ADAPTER_SRCS := $(LIBBSD_ADAPTER_CORE_SRCS)
endif

ifeq ($(CONFIG_BUS_USB),1)
# The domain bring-up is shared by both backends; the HCD world and the
# platform composition are not: the CherryUSB image must NOT link the imported
# usbdi/hub/ehci/xhci units (the compat layer provides the driver-facing
# symbols, and both stacks define the same HCD entry points).
USB_DOMAIN_OBJS := $(addprefix $(BUILD)/,$(USB_DOMAIN_SRCS:.c=.o))
ifeq ($(CONFIG_USB_BACKEND_CHERRYUSB),1)
USB_HOST_OBJS := $(addprefix $(BUILD)/,$(USB_HOST_SRCS:.c=.o))
CHERRYUSB_OBJS := $(addprefix $(BUILD)/,$(CHERRYUSB_SUB_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/,$(CHERRYUSB_ADAPTER_SRCS:.c=.o)) \
	$(USB_DOMAIN_OBJS)
else
LIBBSD_BSD_SRCS += $(LIBBSD_USB_SRCS)
LIBBSD_ADAPTER_SRCS += $(LIBBSD_USB_PLATFORM_SRCS) $(USB_DOMAIN_SRCS)
USB_HOST_OBJS := $(addprefix $(BUILD)/,$(USB_HOST_SRCS:.c=.o))
endif
ifeq ($(CONFIG_SHELL),1)
USB_HOST_OBJS += $(addprefix $(BUILD)/,$(USB_HOST_CMD_SRCS:.c=.o))
ifeq ($(CONFIG_USB_BACKEND_CHERRYUSB),1)
USB_HOST_OBJS += $(addprefix $(BUILD)/,$(CHERRYUSB_CMD_SRCS:.c=.o))
endif
endif
endif

# The usbdi(9) request compat layer and its class hook (feat/cherryusb_ehci):
# the CherryUSB backend's driver-facing half.  Both compile in the BSD world
# plus the CherryUSB world (the shim implements the usbdi surface over
# CherryUSB's usbh_urb API, so it needs both include sets) - hence the
# explicit rules below rather than the libbsd-directory pattern rules.
ifeq ($(CONFIG_USB_BACKEND_CHERRYUSB)-$(CONFIG_NIC_URTWN),1-1)
LIBBSD_IMPL_SRCS    += port/adapters/cherryusb/usbdi_compat.c
LIBBSD_ADAPTER_SRCS += port/adapters/cherryusb/usbh_urtwn_class.c
endif
ifeq ($(CONFIG_NIC_URTWN),1)
LIBBSD_IMPL_SRCS    += port/adapters/libbsd/urtwn_reg.c
LIBBSD_ADAPTER_SRCS += $(LIBBSD_URTWN_ADAPTER_SRCS)
endif
ifeq ($(CONFIG_NIC_IWM),1)
LIBBSD_ADAPTER_SRCS += $(LIBBSD_PCIE_ADAPTER_SRCS)
endif
ifeq ($(CONFIG_NIC_RTW8189F),1)
LIBBSD_BSD_SRCS     += $(LIBBSD_SDIO_BSD_SRCS)
LIBBSD_IMPL_SRCS    += $(LIBBSD_SDIO_IMPL_SRCS)
LIBBSD_ADAPTER_SRCS += $(LIBBSD_SDIO_ADAPTER_SRCS)
endif
ifeq ($(CONFIG_UVC),1)
LIBBSD_BSD_SRCS     += third-party/libbsd/sys/dev/video.c \
	third-party/libbsd/sys/dev/usb/uvideo.c
LIBBSD_ADAPTER_SRCS += port/adapters/libbsd/av_video.c \
	port/adapters/libbsd/av_dump.c \
	port/adapters/libbsd/uvc_cmds.c
endif

# The audio line: audio.c is the audio(4) middle layer and its converter
# TUs (linear/mulaw/alaw) are link-time dependencies of it; uaudio is the
# USB audio class driver that hands the microphone's endpoints to it.
ifeq ($(CONFIG_UAC),1)
LIBBSD_BSD_SRCS     += third-party/libbsd/sys/dev/audio/audio.c \
	third-party/libbsd/sys/dev/audio/linear.c \
	third-party/libbsd/sys/dev/audio/mulaw.c \
	third-party/libbsd/sys/dev/audio/alaw.c \
	third-party/libbsd/sys/dev/usb/uaudio.c
LIBBSD_ADAPTER_SRCS += port/adapters/libbsd/av_audio.c
endif

LIBBSD_INC := -I$(BUILD) -Iinclude \
	-Iport/adapters/libbsd/compat/netbsd \
	-Ithird-party/libbsd/sys \
	-Ithird-party/libbsd/sys/arch \
	-Iport/adapters/libbsd/osal \
	-Iport/adapters/libbsd/osal/compat \
	-Iport/adapters/libbsd \
	-Iport/adapters/usb \
	-Ithird-party/tlsf

# The feature defines reach the frozen import from here rather than from the
# generated header: a force-included config.h inside the vendored world would
# be the one place the layering rule (only the adapter knows upstream) breaks.
# They are derived from the keys, so the config files stay the single source.
LIBBSD_BSD_CFG := -D_KERNEL -D_KERNEL_OPT \
	-D_COMPAT_SYS_SYSCTL_H_ -include stdarg.h \
	-DUSBHIST_SIZE=$(CONFIG_USBHIST_SIZE) \
	-include port/adapters/libbsd/compat/netbsd/opt_usb.h \
	-include port/adapters/libbsd/port_config_bsd.h
ifeq ($(CONFIG_BSD_DIAGNOSTIC),1)
LIBBSD_BSD_CFG += -DDIAGNOSTIC
endif
ifeq ($(CONFIG_IWM_DEBUG),1)
LIBBSD_BSD_CFG += -DIWM_DEBUG
endif
# IWM_DEBUG compiles in iwm_nic_error()/iwm_nic_umac_error() and the
# tx/rx-ring + 802.11-state dump that run on the fatal-firmware-error
# interrupt (if_iwm.c iwm_softintr). Runtime traces behind it stay gated
# by the iwm_debug variable (default 0), so the only new output is the
# one-shot dump at the fatal moment - without it the SW_ERR branch
# prints a single line and the error id is unrecoverable.
# USBHIST_SIZE is usb.c's history ring (the imported default is 50000
# records, which is ~3 MB of .bss here); 4096 x 64 B keeps a whole
# enumeration trail with room to spare.

# VIDEO_DEBUG=0 (not bare): the middle layer compiles its DPRINTF paths in
# but leaves videodebug=0, so 'uvc dbg <n>' can turn them on at runtime
# without a rebuild.  A bare -DVIDEO_DEBUG would print from the start.
# _KERNEL_OPT makes this build behave like a config(8) kernel for the
# imported sources: every `#ifdef _KERNEL_OPT #include "opt_*.h"` and
# `#include "<device>.h"` fires, so the compat tree's stand-ins for the
# generated headers are the ones used.  Without it those includes are
# skipped silently, and so is the code they guard - a missing
# usb_dma.h meant usbdi.c compiled its DMA buffer path out
# (NUSB_DMA == 0) and every device transfer got a stale buffer address.
# NOTE: editing these flags does not invalidate $(OBJS); rm -rf
# $(BUILD)/third-party/libbsd $(BUILD)/port/adapters/libbsd after a
# change.  Same for adding a header an existing .d file does not list.
# DIAGNOSTIC is on for the bring-up rounds: upstream makes the KASSERT
# family (and the xfer state it inspects - ux_state, ex_isdone) live only
# under DIAGNOSTIC, so without it the asserts this port prints are
# checking state nobody maintains.  No panic() lives inside a DIAGNOSTIC
# block anywhere in the compiled set (audited), and it also switches on
# the DIAGNOSTIC-only prints (uhub's "port %d, device not enabled", the
# ehci xfer dumps).
# The UVC line's own forensics (CONFIG_UVC_DEBUG=1): the imported driver's
# counters, the first-8-packet header dump and EHCI's isoc iTD ctl dump.
# LIBBSD_SUB_CFG is an immediate expansion of LIBBSD_BSD_CFG (it freezes the
# value at its `:=` line), so a switch added after that line has to feed both;
# asserting here that it is empty catches a future key wired to only one.
LIBBSD_SUB_CFG := -w $(LIBBSD_BSD_CFG)
ifeq ($(CONFIG_UVC_DEBUG),1)
LIBBSD_BSD_CFG += -DUVIDEO_DEBUG -DVIDEO_DEBUG=0 -DUVC_PORT_DIAG
LIBBSD_SUB_CFG += -DUVIDEO_DEBUG -DVIDEO_DEBUG=0 -DUVC_PORT_DIAG
endif

# --- lwip + wlan netif bridge (feat/wpa_supplicant + feat/netutils) -------------
# lwIP 2.2.1 (pinned submodule). The set follows upstream src/Filelists.mk for
# the core/IPv4 groups plus the api files the NO_SYS=0 tcpip model needs
# (tcpip.c, netifapi.c, err.c). The netutils feat turned the sequential API
# on: sockets.c/api_lib.c/api_msg.c joined (netconn is the layer sockets.c
# sits on - the lwipopts switches and these files move together) and dns.c
# rides in for ping/ntp name resolution (LWIP_DNS=1). acd.c is in because
# LWIP_ACD follows LWIP_DHCP by default and etharp.c then calls into it;
# autoip.c/igmp.c stay out. The sys_arch
# is the CMSIS twin; lwipopts.h and
# arch/*.h are the adapter's shadow copies and sit ahead of the vendored tree
# on the include path. The wlan netif bridge (net80211/lwip/lwip_netif.c)
# lives next to the port hooks it consumes but compiles in this world: it
# needs no BSD headers, and the BSD cfg's force-included endian.h clashes
# with lwip's htons macros. Upstream sources compile with warnings silenced
# (-w): frozen imports, edited only through patches/.
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
	third-party/lwip/src/core/dns.c \
	third-party/lwip/src/api/netdb.c \
	third-party/lwip/src/api/err.c \
	third-party/lwip/src/api/api_lib.c \
	third-party/lwip/src/api/api_msg.c \
	third-party/lwip/src/api/netbuf.c \
	third-party/lwip/src/api/netifapi.c \
	third-party/lwip/src/api/sockets.c \
	third-party/lwip/src/api/tcpip.c \
	third-party/lwip/src/netif/ethernet.c \
	port/adapters/libbsd/lwip/lwip_netif.c \
	port/adapters/lwip/cmsis/sys_arch.c \
	port/adapters/lwip/lwip_adapter.c \
	port/adapters/lwip/lwip_diag.c \
	port/adapters/lwip/net_cmd.c

LWIP_INC := -I$(BUILD) -Iport/adapters/lwip/include \
	-Iport/adapters/lwip/cmsis/include \
	-Ithird-party/lwip/src/include \
	-Iport/adapters/libbsd

# --- netutils world (feat/netutils) --------------------------------------------
# RT-Thread netutils, vendored (see third-party/netutils/PROVENANCE.md). The
# shim shadow headers (rtthread.h / rtdbg.h / finsh.h / sys/socket.h) come
# FIRST so they win over newlib's declarations-only headers; the BSD socket
# surface resolves against lwIP via the LWIP_INC world. Vendored sources
# compile -w (frozen import, edited only through PROVENANCE-tracked
# deviations); the adapter port files compile with the normal adapter set.
NETUTILS_VENDORED_SRCS := \
	third-party/netutils/ping/ping.c \
	third-party/netutils/tftp/tftp_client.c \
	third-party/netutils/tftp/tftp_server.c \
	third-party/netutils/tftp/tftp_xfer.c \
	third-party/iperf3_embedded/iperf3_embedded.c \
	third-party/netutils/netio/netio.c \
	third-party/netutils/tcpdump/tcpdump.c \

NETUTILS_INC := -I$(BUILD) -Iport/adapters/netutils/shim \
	-Iport/adapters/netutils \
	-Ithird-party/netutils/ping \
	-Ithird-party/netutils/tftp \
	-Ithird-party/netutils/netio \
	-Ithird-party/netutils/tcpdump \
	-Ithird-party/netutils/tcpdump \
	-Ithird-party/iperf3_embedded \
	-Iport/adapters/cherrysh \
	-Ithird-party/cherrysh

# --- wpa_supplicant (feat/wpa_supplicant) ----------------------------------------
# The PSK-only file set (no EAP/WPS/P2P/ctrl-iface/SME), the same list the
# frozen workspace compiled from this fork; upstream sources compile with
# warnings silenced (WPA_CFG's -w): frozen imports, edited only through
# patches/. The whole world gets wpa_port_config.h force-included, which
# steers the fork's includes.h/build_config.h onto the plain-libc path
# (CONFIG_OS_EMBOX) and selects our driver slot in src/drivers/drivers.c
# (CONFIG_DRIVER_EMBOX; the ops live in the adapter's driver_net80211.c).
# The shim/ directory comes first for the wpa-world units: newlib has no
# net/if.h or netinet/in.h, and pulling the net80211 compat shadows instead
# would drag the BSD macro world (kalloc-style malloc) in with them.
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

WPA_INC := -I$(BUILD) -Iport/adapters/wpa_supplicant/shim \
	-Ithird-party/wpa_supplicant \
	-Ithird-party/wpa_supplicant/src \
	-Ithird-party/wpa_supplicant/src/utils \
	-Ithird-party/wpa_supplicant/src/drivers \
	-Ithird-party/wpa_supplicant/src/l2_packet \
	-Ithird-party/wpa_supplicant/wpa_supplicant \
	-Iport/adapters/wpa_supplicant

WPA_CFG := -w -include stdarg.h \
	-include port/adapters/wpa_supplicant/wpa_port_config.h

# --- fsl_sdmmc + dw-mmc world (feat/net80211_sdio) ------------------------------
# The NXP fsl_sdmmc protocol layer (submodule third-party/sdmmc, see
# IMPORT-INFO.md) compiles as a frozen import; the SDK headers it expects
# (fsl_common.h / fsl_os_abstraction.h / fsl_sdmmc_host.h) come from the
# adapter's shadow directory, which the include order places ahead of the
# vendored tree. The SD/eMMC card modules ride along exactly as the frozen
# workspace compiled them - SDIO_Init is the only card entry the adapter
# takes, hostType picks the controller, and the unmoved modules stay dead
# code rather than a fidelity risk. The host controller is first-party
# (drivers/dwc_mmc.c behind include/dwmmc.h); the SDMMCHOST_* surface over
# it is the adapter's sdmmc_host_dwmmc.c.
SDMMC_SRCS := \
	third-party/sdmmc/common/fsl_sdmmc_common.c \
	third-party/sdmmc/mmc/fsl_mmc.c \
	third-party/sdmmc/sd/fsl_sd.c \
	third-party/sdmmc/sdio/fsl_sdio.c

SDMMC_ADAPTER_SRCS := \
	port/adapters/sdmmc/sdmmc_osa.c \
	port/adapters/sdmmc/sdmmc_dispatch.c \
	port/adapters/sdmmc/sdmmc_adapter.c \
	port/adapters/sdmmc/sdmmc_host_dwmmc.c \
	port/adapters/sdmmc/sdmmc_glue_irq.c \
	port/adapters/sdmmc/sdmmc_cmds.c

SDMMC_INC := -I$(BUILD) -Iport/adapters/sdmmc/shadow \
	-Iport/adapters/sdmmc \
	-Ithird-party/sdmmc/common \
	-Ithird-party/sdmmc/sd \
	-Ithird-party/sdmmc/osa \
	-Ithird-party/sdmmc/mmc \
	-Ithird-party/sdmmc/sdio

ifeq ($(CONFIG_BUS_SDIO),1)
SDMMC_OBJS := $(addprefix $(BUILD)/,$(SDMMC_SRCS:.c=.o)) \
	$(addprefix $(BUILD)/,$(SDMMC_ADAPTER_SRCS:.c=.o))
endif

# Board assembly is shared; the kernel-side assembly is the seam itself:
# tx_vectors.S (runtime vector table + SPSel entry stubs) plus the kernel
# port's own assembly.
ASM_SRCS := \
	port/aarch64/startup.S \
	port/aarch64/smp_secondary.S \
	port/adapters/threadx/tx_vectors.S \
	$(THREADX_PORT_SRCS)

# --- rules --------------------------------------------------------------------

C_SRCS := $(KERNEL_SRCS) $(ARCH_SRCS) $(ADAPTER_SRCS) $(DRIVER_SRCS) $(BOARD_SRCS) $(APP_SRCS)
LIBBSD_IMPL_OBJS := $(addprefix $(BUILD)/,$(LIBBSD_IMPL_SRCS:.c=.o))
LIBBSD_ADAPTER_OBJS := $(addprefix $(BUILD)/,$(LIBBSD_ADAPTER_SRCS:.c=.o))
# The three stack worlds ride together with CONFIG_NET: lwIP's netif bridge
# calls the wlan port hooks, wpa runs on net80211 and netutils' sockets
# resolve through lwIP, so a partial set would not link.
ifeq ($(CONFIG_NET),1)
LWIP_OBJS := $(addprefix $(BUILD)/,$(LWIP_SRCS:.c=.o))
NETUTILS_OBJS := $(addprefix $(BUILD)/,$(NETUTILS_VENDORED_SRCS:.c=.o))
WPA_OBJS := $(addprefix $(BUILD)/,$(WPA_CORE_SRCS:.c=.o) $(WPA_PORT_SRCS:.c=.o))
endif
OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o)) $(addprefix $(BUILD)/,$(ASM_SRCS:.S=.o)) \
	$(addprefix $(BUILD)/,$(LIBBSD_BSD_SRCS:.c=.o)) \
	$(LIBBSD_IMPL_OBJS) $(LIBBSD_ADAPTER_OBJS) $(USB_HOST_OBJS) \
	$(CHERRYUSB_OBJS) \
	$(LWIP_OBJS) $(WPA_OBJS) $(NETUTILS_OBJS) $(SDMMC_OBJS)
DEPS := $(OBJS:.o=.d)

# Order-only, for the parallel build: config.h must exist before any object
# compiles (tx_user.h and the adapter shadow headers include it).  Rebuilding
# on a *change* to it needs no rule here - -MMD records it in the .d files of
# every TU that includes it, which is the same mechanism every other header
# uses.
$(OBJS): | $(CONFIG_HDR)

# Kernel and adapters see the vendored trees; board, drivers and app do not.
$(BUILD)/third-party/%.o: third-party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

# The net80211 worlds: submodule sources (-w, frozen upstream) and the
# adapter impl units compile against the compat shadows; the shell-facing
# adapter files additionally see the standard adapter include path.
$(BUILD)/third-party/libbsd/%.o: third-party/libbsd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBBSD_INC) $(LIBBSD_SUB_CFG) -MMD -MP -c $< -o $@

# The CherryUSB compat layer lives in another directory than libbsd's, so the
# pattern rule below cannot match it and it gets its explicit rule further
# down (it needs both include worlds).  Filtering it here keeps make from
# claiming the target with an empty source.
LIBBSD_IMPL_GENERIC_OBJS := $(filter-out %/usbdi_compat.o,$(LIBBSD_IMPL_OBJS))

$(LIBBSD_IMPL_GENERIC_OBJS): $(BUILD)/port/adapters/libbsd/%.o: port/adapters/libbsd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LIBBSD_INC) $(LIBBSD_BSD_CFG) -MMD -MP -c $< -o $@

# The SDIO claim layer, the raw A/V dump, the backend-neutral USB domain
# sequence and the CherryUSB class hook have their own rules below (extra
# include worlds / a different directory than libbsd's); excluding them here
# keeps the static pattern from claiming them and make from warning about an
# overridden recipe.
LIBBSD_ADAPTER_GENERIC_OBJS := $(filter-out %/wlan_sdio_claim.o %/av_dump.o %/usb_domain.o %/usbh_urtwn_class.o,$(LIBBSD_ADAPTER_OBJS))

$(LIBBSD_ADAPTER_GENERIC_OBJS): $(BUILD)/port/adapters/libbsd/%.o: port/adapters/libbsd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(LIBBSD_INC) \
		$(LIBBSD_BSD_CFG) -MMD -MP -c $< -o $@

# the SDIO claim layer additionally sees the fsl_sdio world (the fsl_sdio
# API it binds the bus ops to, and the adapter's board header)
$(BUILD)/port/adapters/libbsd/wlan_sdio_claim.o: port/adapters/libbsd/wlan_sdio_claim.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(LIBBSD_INC) \
		$(SDMMC_INC) -MMD -MP -c $< -o $@

# The raw A/V dump is lwIP-socket code, so it compiles in the lwip world
# (the adapter's lwipopts.h plus the vendored lwip includes) and NOT with
# LIBBSD_BSD_CFG: the BSD world force-includes endian.h, whose macros clash
# with lwip's htons (same reason the lwip netif bridge keeps to this set).
# An explicit rule is required because the longer libbsd pattern rule would
# otherwise win the stem match and hand it the BSD config.
$(BUILD)/port/adapters/libbsd/av_dump.o: port/adapters/libbsd/av_dump.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(LIBBSD_INC) \
		$(LWIP_INC) -MMD -MP -c $< -o $@

# The usbdi(9) compat layer is the one file that spans both worlds: it
# implements the imported usbdi surface (usbdivar.h types, the driver-facing
# prototypes) over CherryUSB's usbh_urb API, so it compiles with the BSD
# include set and config PLUS the CherryUSB include set.  The class hook next
# to it is CherryUSB-side and stays in the CherryUSB world.  Both need
# explicit rules: their directory is not the one the libbsd pattern rules
# name, and a pattern rule that cannot match leaves make with no recipe.
$(BUILD)/port/adapters/cherryusb/usbdi_compat.o: port/adapters/cherryusb/usbdi_compat.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(LIBBSD_INC) \
		$(LIBBSD_BSD_CFG) $(CHERRYUSB_INC) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/cherryusb/usbh_urtwn_class.o: port/adapters/cherryusb/usbh_urtwn_class.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(CHERRYUSB_INC) \
		-MMD -MP -c $< -o $@

# The usb host abstraction's NetBSD backend is the one file above the platform
# that includes the imported usbdi world, so it compiles with the BSD include
# set and config (same world as the platform files).  The CherryUSB backend and
# the command TU next to it are ordinary adapter code in their own worlds.
$(BUILD)/port/adapters/usb/usb_host_netbsd.o: port/adapters/usb/usb_host_netbsd.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(LIBBSD_INC) \
		$(LIBBSD_BSD_CFG) -MMD -MP -c $< -o $@

# The CherryUSB backend and its command TU see the CherryUSB world; warnings
# silenced on the vendored side only (the adapter files are ours and keep
# -Wall/-Wextra).
$(BUILD)/port/adapters/usb/usb_host_cherryusb.o: port/adapters/usb/usb_host_cherryusb.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(CHERRYUSB_INC) \
		-MMD -MP -c $< -o $@

$(BUILD)/port/adapters/cherryusb/%.o: port/adapters/cherryusb/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(CHERRYUSB_INC) \
		-MMD -MP -c $< -o $@

# The backend-neutral USB domain sequence: board registers only, but
# usb_board.h lives with the NetBSD adapter (it is the single copy of the
# board's USB constants, and the CherryUSB adapter reads it too).
$(BUILD)/port/adapters/usb/usb_domain.o: port/adapters/usb/usb_domain.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -Iport/adapters/libbsd \
		-MMD -MP -c $< -o $@

# The vendored CherryUSB tree itself: -w (frozen upstream, edited only through
# patches/), compiled in its own include world.
$(BUILD)/third-party/cherryusb/%.o: third-party/cherryusb/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(CHERRYUSB_INC) -w -MMD -MP -c $< -o $@

# fsl_sdmmc protocol layer: frozen NXP import, warnings silenced (-w); the
# shadow SDK headers come first so they win over anything vendored.
$(BUILD)/third-party/sdmmc/%.o: third-party/sdmmc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(SDMMC_INC) -w -MMD -MP -c $< -o $@

# The sdmmc adapter: board primitives + the dw-mmc driver interface
# (include/dwmmc.h) + the shadow/vendored fsl headers.
$(BUILD)/port/adapters/sdmmc/%.o: port/adapters/sdmmc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(SDMMC_INC) -MMD -MP -c $< -o $@

# lwIP world: the pinned upstream core compiles in its own include world
# (adapter shadows first, so lwipopts.h/arch/*.h win), warnings silenced.
$(BUILD)/third-party/lwip/%.o: third-party/lwip/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LWIP_INC) -w -MMD -MP -c $< -o $@

# mpaland/printf (vendored, third-party/printf): float/exponential rendering
# is compiled out - the image is -mgeneral-regs-only, there is no FP state to
# format into, such specifiers degrade like unknown ones. See PROVENANCE.md.
$(BUILD)/third-party/printf/printf.o: third-party/printf/printf.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DPRINTF_DISABLE_SUPPORT_FLOAT -DPRINTF_DISABLE_SUPPORT_EXPONENTIAL \
		$(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

# The netutils vendored world: shim shadows first, then the lwip world (the
# shim's sys/socket.h reaches lwip/sockets.h through this path).
$(BUILD)/third-party/netutils/%.o: third-party/netutils/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NETUTILS_INC) $(LWIP_INC) -w -MMD -MP -c $< -o $@

# The iperf3_embedded vendored world: same include set (its OS glue is the
# netutils adapter's iperf3_port.h; sockets resolve against lwIP).
$(BUILD)/third-party/iperf3_embedded/%.o: third-party/iperf3_embedded/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NETUTILS_INC) $(LWIP_INC) -w -MMD -MP -c $< -o $@

# Generic adapter code (lwip adapter files included) sees the adapter and the
# lwip include worlds; net80211-specific files match the longer patterns above.
# The netutils adapter files additionally see the shim + vendored headers.
$(BUILD)/port/adapters/%.o: port/adapters/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(NETUTILS_INC) $(LWIP_INC) -MMD -MP -c $< -o $@

# The wpa worlds. Submodule sources and the adapter glue compile in the same
# wpa world; driver_net80211 and l2_packet_net80211 additionally see the
# net80211/BSD world (they call net80211 directly and link into the
# supplicant - the compat shadows must precede the wpa include set for them).
$(BUILD)/third-party/wpa_supplicant/%.o: third-party/wpa_supplicant/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/wpa_supplicant/%.o: port/adapters/wpa_supplicant/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/wpa_supplicant/driver_net80211.o: port/adapters/wpa_supplicant/driver_net80211.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(LIBBSD_INC) $(LIBBSD_BSD_CFG) \
		$(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/wpa_supplicant/l2_packet_net80211.o: port/adapters/wpa_supplicant/l2_packet_net80211.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(LIBBSD_INC) $(LIBBSD_BSD_CFG) \
		$(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

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

$(TARGET).elf: $(OBJS) | $(CONFIG_HDR) $(CONFIG_STAMP)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@printf 'image: %s (%s bytes)\n' $@ "$$(stat -c%s $@)"
	@sha256sum $@
	@printf 'config: CONFIG=%s (%s)\n' "$(CONFIG)" "$(abspath $(CONFIG_HDR))"

# --- generated configuration header ------------------------------------------
# configs/config.h.in with every @KEY@ replaced by the resolved value.  Values
# are substituted with $(foreach) + sed rather than make's own $(subst) so a
# value containing characters make would treat specially (the -Og profile is a
# build flag and not in the template, but a future string key could be) is
# never re-parsed.  The file is only rewritten when the content changes, so an
# unchanged config does not invalidate every object through -MMD.
CONFIG_HDR_VARS := $(foreach k,$(CONFIG_HDR_KEYS),-e 's|@$(k)@|$(subst |,|,$($(k)))|g')

# The stamp is a prerequisite of the header so a key mismatch is reported
# before config.h is rewritten: the tree keeps the key set its objects were
# actually built with until the clean happens.
# FORCE so a key change is re-examined every run; the cmp keeps the mtime
# (and therefore the objects that include it) untouched when nothing changed.
$(CONFIG_HDR): $(CONFIG_DIR)/config.h.in $(CONFIG_FILE) $(CONFIG_STAMP) FORCE
	@mkdir -p $(BUILD)
	@sed $(CONFIG_HDR_VARS) $< > $@.new
	@if [ -f $@ ] && cmp -s $@ $@.new; then rm -f $@.new; else \
		mv -f $@.new $@; \
		echo "config: $(CONFIG_HDR) updated (CONFIG=$(CONFIG))"; \
	fi

# --- stale-object guard ------------------------------------------------------
# The -D defines and the vendored-world flags are invisible to the dependency
# files, so an incremental tree that changed keys links objects from two
# different images (2026-09-27: half iwm, half urtwn, undefined iwm_ca/usb_cd).
# The stamp is the whole resolved key set; a mismatch is a hard, self-explaining
# error instead.  Switching CONFIG= picks another build directory and needs no
# clean; changing a key inside one config does.
$(CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' "$(foreach k,$(CONFIG_KEYS),$(k)=$($(k)))" | sha256sum | cut -d' ' -f1 > $@.new
	@if [ -f $@ ] && ! cmp -s $@ $@.new; then \
		echo "ERROR: $(BUILD) holds objects built with a different configuration; run 'make clean-config' (or 'make clean') before changing keys in CONFIG=$(CONFIG)"; \
		echo "       the previous key set differs from the current one; 'make show-config' prints this one"; \
		rm -f $@.new; exit 1; \
	fi
	@mv -f $@.new $@

# Without this the first explicit target (the ELF rule) would be the default,
# so a bare `make` builds no image at all - which silently leaves a stale
# binary on the TFTP root and makes a failed rebuild look like a boot failure.
.DEFAULT_GOAL := all

.PHONY: all deploy modules sync gates clean clean-config configs show-config check-config FORCE

all: $(CONFIG_HDR) $(CONFIG_STAMP) $(TARGET).bin

# The available configurations, and what this tree would build right now.
configs:
	@printf 'available configurations (%s/*.conf):\n' "$(CONFIG_DIR)"
	@for f in $(CONFIG_DIR)/*.conf; do \
		name=$$(basename "$$f" .conf); \
		desc=$$(sed -n '1s/^# //p' "$$f"); \
		case "$$name" in \
		base) printf '  %-10s %s [the key table; every config includes it]\n' "$$name" "$$desc"; continue ;; \
		esac; \
		if [ "$$name" = "$(CONFIG)" ]; then mark=' <- current (CONFIG=$(CONFIG))'; else mark=''; fi; \
		printf '  %-10s %s%s\n' "$$name" "$$desc" "$$mark"; \
	done
	@printf '\nfragments (%s/fragments/*.conf):\n' "$(CONFIG_DIR)"
	@for f in $(CONFIG_DIR)/fragments/*.conf; do \
		printf '  %-16s %s\n' "$$(basename "$$f" .conf)" "$$(sed -n '1s/^# //p' "$$f")"; \
	done

show-config:
	@printf 'CONFIG=%s  (build directory: %s)\n\n' "$(CONFIG)" "$(BUILD)"
	@printf 'keys:\n'
	@$(foreach k,$(CONFIG_KEYS),printf '  %-30s %s\n' '$(k)' '$($(k))';)
	@printf '\nartifacts:\n  binary  %s\n  elf     %s\n  config  %s\n' \
		"$(TARGET).bin" "$(TARGET).elf" "$(CONFIG_HDR)"

# Guard against the three key lists drifting apart: the table (configs/*.conf),
# the Makefile's CONFIG_KEYS, and the config.h.in template.
check-config:
	@fail=0; \
	for f in $(CONFIG_DIR)/*.conf $(CONFIG_DIR)/fragments/*.conf; do \
		[ -e "$$f" ] || continue; \
		for key in $$(sed -n 's/^\(CONFIG_[A-Z0-9_]*\)[[:space:]]*[:?]*=.*/\1/p' "$$f"); do \
			case " $(CONFIG_KEYS) " in *" $$key "*) ;; \
			*) echo "  FAIL  $$f sets unknown key $$key"; fail=1 ;; esac; \
		done; \
	done; \
	for key in $(CONFIG_HDR_KEYS); do \
		grep -q "@$$key@" $(CONFIG_DIR)/config.h.in || { echo "  FAIL  config.h.in is missing @$$key@"; fail=1; }; \
	done; \
	for tok in $$(sed -n 's/.*@\(CONFIG_[A-Z0-9_]*\)@.*/\1/p' $(CONFIG_DIR)/config.h.in); do \
		case " $(CONFIG_HDR_KEYS) " in *" $$tok "*) ;; \
		*) echo "  FAIL  config.h.in has @$$tok@ which is not a known key"; fail=1 ;; esac; \
	done; \
	if [ "$$fail" -eq 0 ]; then echo 'check-config: PASS'; else echo 'check-config: FAIL'; exit 1; fi

# Copy to the TFTP root under the name the board's boot profile expects
# (oslab `rtos` profile -> rtos.bin; the banner names the configuration so
# images from different configs are tellable apart in a terminal log).
# Records the hash before and after so the transfer is verifiable.
deploy: $(TARGET).bin
	@printf 'before: '; sha256sum /mnt/d/tftpboot/rtos.bin 2>/dev/null || echo '(absent)'
	cp $(TARGET).bin /mnt/d/tftpboot/rtos.bin
	@printf 'after : '; sha256sum /mnt/d/tftpboot/rtos.bin
	@printf 'local : '; sha256sum $(TARGET).bin

# Submodules + patches (policy: IMPORT-INFO.md and patches/README.md).
# Idempotent: a patch that no longer applies cleanly is reported and kept.
#
# third-party/sdmmc still records a local-path URL, so the file:// protocol
# must be allowed for the whole invocation (-c beats any stale local config
# ordering problem on a fresh clone). third-party/libbsd records the fork
# URL (git@github.com:KevinACoder/netbsd-src.git); its fwc/libbsd branch is
# pushed there, so the recorded gitlink resolves from the recorded URL.
# The libbsd working tree is kept sparse (the net80211 + usb + urtwn subset
# instead of the full ~7 GB src tree); sparse-checkout only rewrites the
# submodule's working tree, the recorded gitlink is untouched. Non-cone
# patterns on purpose: cone mode cannot express "files directly inside
# sys/dev" (video.c, audio.c, auconv.c - the video/audio middle layer the
# UVC line compiles), and a file path (sys/fs/unicode.h) fails a cone "set"
# so a fresh clone silently keeps the full tree, where the imported sys/sys/
# headers shadow the compat/netbsd ones and the build breaks. The set below
# is the shape the checked-out lanes actually build with.
modules:
	git -c protocol.file.allow=always submodule update --init --recursive
	git -C third-party/libbsd sparse-checkout set --no-cone \
		'/*' '!/*/' \
		'/external/' '!/external/*/' '/external/realtek/' '!/external/realtek/*/' \
		'/sys/' '!/sys/*/' '/sys/arch/' '!/sys/arch/*/' \
		'/sys/arch/arm/' '!/sys/arch/arm/*/' \
		'/sys/crypto/' '!/sys/crypto/*/' '/sys/dev/' '!/sys/dev/*/' \
		'/external/realtek/rtw8189f/' '/external/realtek/urtwn/' \
		'/sys/arch/arm/include/' '/sys/crypto/aes/' '/sys/dev/hid/' \
		'/sys/dev/ic/' '/sys/dev/pci/' '/sys/dev/sdmmc/' '/sys/dev/usb/' \
		'/sys/dev/audio/' \
		'/sys/fs/' '/sys/net80211/' '/sys/sys/videoio.h' \
		'/sys/sys/audioio.h' \
		'/sys/sys/featuretest.h' '/sys/compat/sys/time.h' \
		'/sys/compat/sys/time_types.h' || \
		echo 'note: libbsd sparse-checkout not set (kept full checkout)'
	git -C third-party/wpa_supplicant sparse-checkout set \
		src wpa_supplicant || \
		echo 'note: wpa_supplicant sparse-checkout not set (kept full checkout)'
	# Non-cone patterns: the full fsl_sdmmc tree carries the standalone
	# SDK's common/fsl_common.h and osa/fsl_os_abstraction.h, whose
	# relative-quote lookup (from a same-directory includer) beats the
	# shadow include path (the frozen workspace vendored the tree WITHOUT
	# those two files for exactly this reason). The shadow copies are what
	# this integration compiles against; the vendored files stay in the
	# submodule, just not in the working tree.
	git -C third-party/sdmmc sparse-checkout set --no-cone \
		'/*' '!/common/fsl_common.h' '!/osa/fsl_os_abstraction.h' || \
		echo 'note: sdmmc sparse-checkout not set (kept full checkout)'
	# CherryUSB: the upstream tree carries demo/, tests/, docs/ (81 MB) and a
	# bundled third_party/ (NimBLE, mbedtls, ...) that this image never
	# compiles.  Both are dropped here for two reasons: the checkout goes from
	# ~105 MB to a few hundred KB, and check-deps.sh's reverse-dependency scan
	# walks third-party/ - bundled NimBLE ships `#include "hal/hal_timer.h"`,
	# which reads as a project-header include and fails the gate for code that
	# is not in any image.  What stays is exactly what the Makefile compiles:
	# core, common, the hub class, the EHCI port, the xHCI port (our own
	# patch-added files - port/xhci's first-level subdirectory is the closed
	# vendor blob drop, excluded wholesale), and the OSAL (kept whole as the
	# reference our adapter copy was derived from).
	git -C third-party/cherryusb sparse-checkout set --no-cone \
		'/*' '!/*/' \
		'/core/' '/common/' \
		'/class/' '!/class/*/' '/class/hub/' \
		'/port/' '!/port/*/' '/port/ehci/' \
		'/port/xhci/' '!/port/xhci/*/' \
		'/osal/' || \
		echo 'note: cherryusb sparse-checkout not set (kept full checkout)'
	@for p in patches/*/*.patch; do \
		[ -e "$$p" ] || continue; \
		comp=$$(printf '%s' "$$p" | cut -d/ -f2); \
		if git -C third-party/$$comp apply --check "$$PWD/$$p" 2>/dev/null; then \
			git -C third-party/$$comp apply "$$PWD/$$p" && echo "applied $$p"; \
		else \
			echo "kept    $$p (already applied or inapplicable)"; \
		fi; \
	done

# First-time setup for a FRESH WORKTREE: seed the per-worktree submodule
# clones from an existing checkout instead of re-downloading them.
#
# Submodule clones are per-worktree, so a new worktree's `make modules` starts
# by cloning every submodule from its recorded URL - for third-party/libbsd
# that is the multi-GB netbsd-src fork over SSH (>25 min measured, aborted).
# The helper points those URLs at a local checkout for THIS worktree only and
# pre-seeds shared clones (alternates, no object copies), which brings the
# whole bootstrap to ~1 s + the checkout.  See tools/wt-submodules.sh.
#
# Only needed once per worktree; plain `make modules` is the steady-state
# command afterwards.
wt-modules:
	./tools/wt-submodules.sh
	$(MAKE) modules

# End-of-round hygiene: materialize each patched submodule's applied tree as
# a commit on branch fwc/<component> so the parent repo's final status is
# clean (no "modified content").  patches/<component>/ stays the source of
# truth: the fwc commit is the mechanical application of those patches on
# top of the pin, and its parent IS the pin.  Upstream-origin submodules
# (threadx/cherrysh/wpa_supplicant) never get their fwc branch pushed;
# third-party/libbsd is the exception - fwc/libbsd is pushed to the fork
# (git@github.com:KevinACoder/netbsd-src.git) so the recorded gitlink stays
# resolvable from the recorded URL.  Run after changing patches or bumping a
# pin, then include the staged gitlink bump in the round's parent commit.
sync: modules
	@for comp in $$(ls -d patches/*/ 2>/dev/null | xargs -n1 basename); do \
		[ -d third-party/$$comp ] || { echo "skip    $$comp (no submodule)"; continue; }; \
		if [ -n "$$(git -C third-party/$$comp status --porcelain)" ]; then \
			branch=$$(git -C third-party/$$comp rev-parse --abbrev-ref HEAD); \
			if [ "$$branch" = "fwc/$$comp" ]; then \
				pin=$$(git -C third-party/$$comp rev-list HEAD | while read c; do \
					s=$$(git -C third-party/$$comp log --format=%s -1 $$c); \
					case "$$s" in \
					"fwc: materialize"*) ;; \
					*) echo $$c; break ;; \
					esac; done | head -1); \
				echo "rebase   $$comp materialization onto pin $$(echo $$pin | cut -c1-10)"; \
				git -C third-party/$$comp checkout -q -f --detach $$pin; \
				for p in patches/$$comp/*.patch; do \
					git -C third-party/$$comp apply "$$PWD/$$p" || exit 1; \
				done; \
			else \
				pin=$$(git -C third-party/$$comp rev-parse --short HEAD); \
			fi; \
			git -C third-party/$$comp checkout -q -B fwc/$$comp; \
			git -C third-party/$$comp add -A; \
			git -C third-party/$$comp commit -q -m \
				"fwc: materialize patches on pin $$pin (generated by make sync; source of truth: patches/$$comp/ in the superproject)"; \
			echo "committed fwc/$$comp on pin $$pin"; \
			git add third-party/$$comp; \
		else \
			echo "clean    third-party/$$comp"; \
		fi; \
	done
	@git status --short

# The merge gates: vendor-trace scan + dependency-direction check.
gates:
	./tools/cleanroom-scan.sh
	./tools/check-deps.sh

clean:
	rm -rf build

# Drop only this configuration's objects: the fix for a key change inside one
# config (see the stamp guard), without disturbing the other configs' builds.
clean-config:
	rm -rf $(BUILD)
	@printf 'removed %s (CONFIG=%s)\n' "$(BUILD)" "$(CONFIG)"

-include $(DEPS)
