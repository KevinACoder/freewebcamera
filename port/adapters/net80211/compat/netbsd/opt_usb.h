/* config(8)-generated: the bring-up rounds run with the usb history
 * logs on (level set through USB_DEBUG_DEFAULT / EHCI_DEBUG_DEFAULT /
 * usbdebug); flip to empty for the quiet production shape.
 *
 * This file is force-included by NET80211_BSD_CFG: upstream reaches it
 * through `#ifdef _KERNEL_OPT #include "opt_usb.h"`, and those defines
 * are not on this port's command line. */
#define USB_DEBUG 1
#define USB_DEBUG_DEFAULT 10
#define EHCI_DEBUG 1
#define EHCI_DEBUG_DEFAULT 10