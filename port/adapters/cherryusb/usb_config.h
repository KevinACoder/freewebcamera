/*
 * @file   usb_config.h
 * @brief  CherryUSB configuration (shadow header), resolved ahead of the
 *         vendored tree by the adapter include order.
 *
 * Values are the board-verified set from the standalone line (os/freertos,
 * four-port USB host, 2026-09-05 runs) trimmed to what this image runs:
 * two EHCI buses, hub class only. The two dcache switches are load-bearing:
 * this image has no .noncacheable MMU window, so the EHCI descriptor pools
 * sit in cacheable .bss and every descriptor access needs explicit
 * maintenance - without them the async list silently stops advancing
 * (measured as iaad timeout / "get device descriptor -14" on this silicon).
 *
 * Baseline: cherry-embedded/CherryUSB master 0e40349b (2026-09-18).
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#ifndef FREEWEBCAMERA_USB_CONFIG_H
#define FREEWEBCAMERA_USB_CONFIG_H

#include <stddef.h>

/* Adapter-provided console formatter (vsnprintf + polled UART). Kept as a
 * function, not a macro expansion of printf: this image has no libc. */
void usbh_console_printf(const char *fmt, ...);
#define CONFIG_USB_PRINTF(...)		usbh_console_printf(__VA_ARGS__)

/* Error-level only: enumeration reporting is the adapter's event handler's
 * job, and the console drops bytes under load. (The xHCI root-cause round
 * of 2026-09-20 temporarily ran LOG; reverted to ERROR for acceptance.) */
#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL		USB_DBG_ERROR
#endif

/* Cache line is 64B on the A55. CherryUSB contract: every DMA buffer is
 * aligned to this (USB_MEM_ALIGNX / USB_ALIGN_UP), otherwise one buffer's
 * boundary shares a cache line with a neighbour and the invalidate side of
 * the discipline clobbers it. */
#ifndef CONFIG_USB_ALIGN_SIZE
#define CONFIG_USB_ALIGN_SIZE		64
#endif

/* No dedicated non-cacheable section exists in this image: the pools stay
 * in cacheable .bss and CONFIG_USB_EHCI_DESC_DCACHE_ENABLE below does the
 * per-descriptor maintenance instead. */
#define USB_NOCACHE_RAM_SECTION

/* --- USB host stack ------------------------------------------------------- */

#define CONFIG_USBHOST_MAX_RHPORTS	8
#define CONFIG_USBHOST_MAX_EXTHUBS	4
#define CONFIG_USBHOST_MAX_EHPORTS	8
#define CONFIG_USBHOST_MAX_INTERFACES	8
/* The AIC8800D80's configuration carries more than the 8 alternate settings
 * the default admits; with 8 its parse fails and the device never enumerates
 * (measured 2026-09-19: "Interface altsetting num 8 overflow", port dead). */
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS	16
#define CONFIG_USBHOST_MAX_ENDPOINTS	8
#define CONFIG_USBHOST_DEV_NAMELEN	16

/* PSC (hub) thread: priority 0 maps to configMAX_PRIORITIES-1 in the vendored
 * osal. It sleeps on its message queue between events, so the top priority
 * costs nothing - the standalone line ran the same value on one core. */
#ifndef CONFIG_USBHOST_PSC_PRIO
#define CONFIG_USBHOST_PSC_PRIO		0
#endif
#ifndef CONFIG_USBHOST_PSC_STACKSIZE
#define CONFIG_USBHOST_PSC_STACKSIZE	8192
#endif

#ifndef CONFIG_USBHOST_MSOS_VENDOR_CODE
#define CONFIG_USBHOST_MSOS_VENDOR_CODE	0x00
#endif

/* ep0 buffer also bounds the config-descriptor read: the AIC8800D80's
 * configuration is 1433 bytes, well past the 512 default (the vendored
 * overflow check refuses longer reads). */
#ifndef CONFIG_USBHOST_REQUEST_BUFFER_LEN
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN	2048
#endif

#ifndef CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT	500
#endif

/* Four buses: EHCI0 @ 0xFD800000, EHCI1 @ 0xFD880000, xHCI(DWC3 host)
 * @ 0xFD000000, xHCI(DWC3 otg-as-host) @ 0xFCC00000. EHCI-only images
 * (EHCI_ONLY=1) still size the arrays for four - the per-bus roothub
 * structures are the bulk of the cost and sit in .bss either way. */
#ifndef CONFIG_USBHOST_MAX_BUS
#define CONFIG_USBHOST_MAX_BUS		4
#endif

/* --- EHCI (rk3568 usb2host0/1) --------------------------------------------- */

/* Capability registers are at the controller base (no vendor offset). */
#ifndef CONFIG_USB_EHCI_HCCR_OFFSET
#define CONFIG_USB_EHCI_HCCR_OFFSET	0
#endif

#ifndef CONFIG_USB_EHCI_FRAME_LIST_SIZE
#define CONFIG_USB_EHCI_FRAME_LIST_SIZE	1024
#endif

#ifndef CONFIG_USB_EHCI_QH_NUM
#define CONFIG_USB_EHCI_QH_NUM		16
#endif

/* Every concurrent QH needs at least 3 qTDs (setup+data+status). */
#ifndef CONFIG_USB_EHCI_QTD_NUM
#define CONFIG_USB_EHCI_QTD_NUM		(CONFIG_USB_EHCI_QH_NUM * 3)
#endif

/* Standard EHCI 1.0: software sets CONFIGFLAG after reset to take the root
 * ports from the (absent) companion controllers. */
#define CONFIG_USB_EHCI_CONFIGFLAG	1

/* The dcache pair - see the file comment. ALIGN 64 pulls the descriptor
 * maintenance up to cache-line granularity inside the driver. */
#define CONFIG_USB_DCACHE_ENABLE	1
#define CONFIG_USB_EHCI_DESC_DCACHE_ENABLE	1

/* --- xHCI (DWC3 usbhost_dwc3 @ 0xFD000000 + otg @ 0xFCC00000) -------------- */

/* Ring sizes (NetBSD xhci.c uses 256/256; the transfer ring matches, the
 * command/event rings are aligned to it now that the driver is the NetBSD
 * port - the old 64-TRB values predate the D38 rewrite). */
#ifndef CONFIG_USBHOST_XHCI_EVENT_RING_SIZE
#define CONFIG_USBHOST_XHCI_EVENT_RING_SIZE	256
#endif
#ifndef CONFIG_USBHOST_XHCI_COMMAND_RING_SIZE
#define CONFIG_USBHOST_XHCI_COMMAND_RING_SIZE	256
#endif
#ifndef CONFIG_USBHOST_XHCI_TRANSFER_RING_SIZE
#define CONFIG_USBHOST_XHCI_TRANSFER_RING_SIZE	256
#endif

#endif /* FREEWEBCAMERA_USB_CONFIG_H */
