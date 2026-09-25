/* config(8)-generated: the bring-up rounds run with the usb history
 * logs on (level set through USB_DEBUG_DEFAULT / EHCI_DEBUG_DEFAULT /
 * usbdebug); flip to empty for the quiet production shape.
 *
 * Reached two ways now that _KERNEL_OPT is defined: upstream's
 * `#ifdef _KERNEL_OPT #include "opt_usb.h"`, and the force-include in
 * NET80211_BSD_CFG (for the adapter units that never include it).  The
 * guard keeps the double inclusion quiet.
 *
 * USB_DEBUG is this port's own divergence: the reference RK3568 kernel
 * runs without it (its generated opt_usb.h has no define), which is why
 * the history machinery had to be switched on deliberately here. */

#ifndef _OPT_USB_H_
#define _OPT_USB_H_

#define USB_DEBUG 1
#define USB_DEBUG_DEFAULT 10
#define EHCI_DEBUG 1
#define EHCI_DEBUG_DEFAULT 10
#define XHCI_DEBUG 1
#define XHCI_DEBUG_DEFAULT 10
/* the bus provides 8-byte accessors and the board maps the controller
 * Device-nGnRE, so the driver's Qword register writes (CRCR et al) go
 * out as single 8-byte stores - the two-Dword fallback leaves the
 * command ring unprogrammed (CRCR ignores half-writes per xHCI 5.4.11) */
#define XHCI_USE_BUS_SPACE_8 1

#endif /* _OPT_USB_H_ */