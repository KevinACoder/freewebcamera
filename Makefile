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
	port/adapters/netutils/netutils_shim.c \
	port/adapters/netutils/tftp_port.c \

DRIVER_SRCS := drivers/uart_ns16550.c

APP_SRCS := app/main.c app/dbg_scenario.c

# --- net80211 + usb world (feat/net80211) -------------------------------------
# The NetBSD import (third-party/net80211, see IMPORT-INFO.md) compiles in
# its own world: the compat shadow headers stand in for the NetBSD kernel
# headers (compat first, so the shadows win over anything the sparse tree
# carries), port_config_bsd.h force-includes the preamble, and the if_urtwn
# driver compiles inside port/adapters/net80211/urtwn_reg.c so its static
# CFATTACH glue stays intact. The adapter impl units (bus_dma/autoconf/osal
# backends) compile in the same world; the shell-facing adapter files compile
# like any other adapter code.

NET80211_BSD_SRCS := \
	third-party/net80211/sys/net80211/ieee80211.c \
	third-party/net80211/sys/net80211/ieee80211_amrr.c \
	third-party/net80211/sys/net80211/ieee80211_crypto.c \
	third-party/net80211/sys/net80211/ieee80211_crypto_ccmp.c \
	third-party/net80211/sys/net80211/ieee80211_crypto_none.c \
	third-party/net80211/sys/net80211/ieee80211_input.c \
	third-party/net80211/sys/net80211/ieee80211_netbsd.c \
	third-party/net80211/sys/net80211/ieee80211_node.c \
	third-party/net80211/sys/net80211/ieee80211_output.c \
	third-party/net80211/sys/net80211/ieee80211_proto.c \
	third-party/net80211/sys/crypto/aes/aes_bear.c \
	third-party/net80211/sys/crypto/aes/aes_ccm.c \
	third-party/net80211/sys/crypto/aes/aes_ccm_mbuf.c \
	third-party/net80211/sys/crypto/aes/aes_ct.c \
	third-party/net80211/sys/crypto/aes/aes_ct_dec.c \
	third-party/net80211/sys/crypto/aes/aes_ct_enc.c \
	third-party/net80211/sys/dev/usb/usbdi.c \
	third-party/net80211/sys/dev/usb/usbdi_util.c \
	third-party/net80211/sys/dev/usb/usb_mem.c \
	third-party/net80211/sys/dev/usb/usb_subr.c \
	third-party/net80211/sys/dev/usb/usb.c \
	third-party/net80211/sys/dev/usb/usb_quirks.c \
	third-party/net80211/sys/dev/usb/uhub.c \
	third-party/net80211/sys/dev/usb/usbroothub.c \
	third-party/net80211/sys/dev/usb/ehci.c

NET80211_IMPL_SRCS := \
	port/adapters/net80211/aes_impl_compat.c \
	port/adapters/net80211/bsd_bus.c \
	port/adapters/net80211/bsd_autoconf.c \
	port/adapters/net80211/bsd_kernhist.c \
	port/adapters/net80211/osal/osal_cmsis_rtos2.c \
	port/adapters/net80211/osal/firmware_cmsis.c \
	port/adapters/net80211/net/bsd_mbuf.c \
	port/adapters/net80211/net/bsd_ifnet.c \
	port/adapters/net80211/urtwn_reg.c

NET80211_ADAPTER_SRCS := \
	port/adapters/net80211/wlan_adapter.c \
	port/adapters/net80211/wlan_console.c \
	port/adapters/net80211/wlan_cmds.c \
	port/adapters/net80211/usb_platform.c \
	port/adapters/net80211/fw_rtl8188eufw.c

NET80211_INC := -Iinclude \
	-Iport/adapters/net80211/compat/netbsd \
	-Ithird-party/net80211/sys \
	-Iport/adapters/net80211/osal \
	-Iport/adapters/net80211/osal/compat \
	-Iport/adapters/net80211 \
	-Ithird-party/tlsf

NET80211_BSD_CFG := -D_KERNEL -D_KERNEL_OPT -DDIAGNOSTIC \
	-D_COMPAT_SYS_SYSCTL_H_ -include stdarg.h \
	-DUSBHIST_SIZE=4096 -include port/adapters/net80211/compat/netbsd/opt_usb.h \
	-include port/adapters/net80211/port_config_bsd.h
# USBHIST_SIZE is usb.c's history ring (the imported default is 50000
# records, which is ~3 MB of .bss here); 4096 x 64 B keeps a whole
# enumeration trail with room to spare.
# _KERNEL_OPT makes this build behave like a config(8) kernel for the
# imported sources: every `#ifdef _KERNEL_OPT #include "opt_*.h"` and
# `#include "<device>.h"` fires, so the compat tree's stand-ins for the
# generated headers are the ones used.  Without it those includes are
# skipped silently, and so is the code they guard - a missing
# usb_dma.h meant usbdi.c compiled its DMA buffer path out
# (NUSB_DMA == 0) and every device transfer got a stale buffer address.
# NOTE: editing these flags does not invalidate $(OBJS); rm -rf
# $(BUILD)/third-party/net80211 $(BUILD)/port/adapters/net80211 after a
# change.  Same for adding a header an existing .d file does not list.
# DIAGNOSTIC is on for the bring-up rounds: upstream makes the KASSERT
# family (and the xfer state it inspects - ux_state, ex_isdone) live only
# under DIAGNOSTIC, so without it the asserts this port prints are
# checking state nobody maintains.  No panic() lives inside a DIAGNOSTIC
# block anywhere in the compiled set (audited), and it also switches on
# the DIAGNOSTIC-only prints (uhub's "port %d, device not enabled", the
# ehci xfer dumps).
# the pinned upstream sources compile with warnings silenced (-w): they are
# frozen imports, edited only through patches/
NET80211_SUB_CFG := -w $(NET80211_BSD_CFG)

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
	port/adapters/net80211/lwip/lwip_netif.c \
	port/adapters/lwip/cmsis/sys_arch.c \
	port/adapters/lwip/lwip_adapter.c \
	port/adapters/lwip/lwip_diag.c \
	port/adapters/lwip/net_cmd.c

LWIP_INC := -Iport/adapters/lwip/include \
	-Iport/adapters/lwip/cmsis/include \
	-Ithird-party/lwip/src/include \
	-Iport/adapters/net80211

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

NETUTILS_INC := -Iport/adapters/netutils/shim \
	-Ithird-party/netutils/ping \
	-Ithird-party/netutils/tftp \
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

WPA_INC := -Iport/adapters/wpa_supplicant/shim \
	-Ithird-party/wpa_supplicant \
	-Ithird-party/wpa_supplicant/src \
	-Ithird-party/wpa_supplicant/src/utils \
	-Ithird-party/wpa_supplicant/src/drivers \
	-Ithird-party/wpa_supplicant/src/l2_packet \
	-Ithird-party/wpa_supplicant/wpa_supplicant \
	-Iport/adapters/wpa_supplicant

WPA_CFG := -w -include stdarg.h \
	-include port/adapters/wpa_supplicant/wpa_port_config.h

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
NET80211_IMPL_OBJS := $(addprefix $(BUILD)/,$(NET80211_IMPL_SRCS:.c=.o))
NET80211_ADAPTER_OBJS := $(addprefix $(BUILD)/,$(NET80211_ADAPTER_SRCS:.c=.o))
LWIP_OBJS := $(addprefix $(BUILD)/,$(LWIP_SRCS:.c=.o))
NETUTILS_OBJS := $(addprefix $(BUILD)/,$(NETUTILS_VENDORED_SRCS:.c=.o))
WPA_OBJS := $(addprefix $(BUILD)/,$(WPA_CORE_SRCS:.c=.o) $(WPA_PORT_SRCS:.c=.o))
OBJS := $(addprefix $(BUILD)/,$(C_SRCS:.c=.o)) $(addprefix $(BUILD)/,$(ASM_SRCS:.S=.o)) \
	$(addprefix $(BUILD)/,$(NET80211_BSD_SRCS:.c=.o)) \
	$(NET80211_IMPL_OBJS) $(NET80211_ADAPTER_OBJS) \
	$(LWIP_OBJS) $(WPA_OBJS) $(NETUTILS_OBJS)
DEPS := $(OBJS:.o=.d)

# Kernel and adapters see the vendored trees; board, drivers and app do not.
$(BUILD)/third-party/%.o: third-party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) -MMD -MP -c $< -o $@

# The net80211 worlds: submodule sources (-w, frozen upstream) and the
# adapter impl units compile against the compat shadows; the shell-facing
# adapter files additionally see the standard adapter include path.
$(BUILD)/third-party/net80211/%.o: third-party/net80211/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NET80211_INC) $(NET80211_SUB_CFG) -MMD -MP -c $< -o $@

$(NET80211_IMPL_OBJS): $(BUILD)/port/adapters/net80211/%.o: port/adapters/net80211/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NET80211_INC) $(NET80211_BSD_CFG) -MMD -MP -c $< -o $@

$(NET80211_ADAPTER_OBJS): $(BUILD)/port/adapters/net80211/%.o: port/adapters/net80211/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(NET80211_INC) \
		$(NET80211_BSD_CFG) -MMD -MP -c $< -o $@

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
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(NET80211_INC) $(NET80211_BSD_CFG) \
		$(WPA_INC) $(WPA_CFG) -MMD -MP -c $< -o $@

$(BUILD)/port/adapters/wpa_supplicant/l2_packet_net80211.o: port/adapters/wpa_supplicant/l2_packet_net80211.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC_COMMON) $(INC_ADAPTER) $(NET80211_INC) $(NET80211_BSD_CFG) \
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

.PHONY: all deploy modules sync gates clean

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
# Its working tree is kept sparse (the net80211 + usb + urtwn subset instead
# of the full ~7 GB src tree); sparse-checkout only rewrites the submodule's
# working tree, the recorded gitlink is untouched. Cone mode takes directory
# paths only - a file path (sys/fs/unicode.h) fails the whole "set" and a
# fresh clone silently keeps the full tree, where the imported sys/sys/
# headers shadow the compat/netbsd ones and the build breaks. The set below
# is the shape the checked-out lanes actually build with.
modules:
	git -c protocol.file.allow=always submodule update --init --recursive
	git -C third-party/net80211 sparse-checkout set \
		sys/net80211 sys/dev/usb sys/dev/ic sys/dev/hid sys/crypto/aes \
		sys/fs external/realtek/urtwn || \
		echo 'note: net80211 sparse-checkout not set (kept full checkout)'
	git -C third-party/wpa_supplicant sparse-checkout set \
		src wpa_supplicant || \
		echo 'note: wpa_supplicant sparse-checkout not set (kept full checkout)'
	@for p in patches/*/*.patch; do \
		[ -e "$$p" ] || continue; \
		comp=$$(printf '%s' "$$p" | cut -d/ -f2); \
		if git -C third-party/$$comp apply --check "$$PWD/$$p" 2>/dev/null; then \
			git -C third-party/$$comp apply "$$PWD/$$p" && echo "applied $$p"; \
		else \
			echo "kept    $$p (already applied or inapplicable)"; \
		fi; \
	done

# End-of-round hygiene: materialize each patched submodule's applied tree as
# a commit on branch fwc/<component> so the parent repo's final status is
# clean (no "modified content").  patches/<component>/ stays the source of
# truth: the fwc commit is the mechanical application of those patches on
# top of the pin, and its parent IS the pin.  Never pushed to the submodule's
# origin.  Run after changing patches or bumping a pin, then include the
# staged gitlink bump in the round's parent commit.
sync: modules
	@for comp in $$(ls -d patches/*/ 2>/dev/null | xargs -n1 basename); do \
		[ -d third-party/$$comp ] || { echo "skip    $$comp (no submodule)"; continue; }; \
		if [ -n "$$(git -C third-party/$$comp status --porcelain)" ]; then \
			pin=$$(git -C third-party/$$comp rev-parse --short HEAD); \
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

-include $(DEPS)
