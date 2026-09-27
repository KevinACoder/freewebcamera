/*
 * @file   usbh_urtwn_class.c
 * @brief  The CherryUSB class hook that claims a wireless dongle and hands
 *         it to the usbdi(9) compat layer, which is what the verbatim
 *         NetBSD urtwn driver attaches against.
 *
 * This is the CherryUSB-side replacement for the NetBSD stack's
 * usb_subr/config_found probe: CherryUSB enumerates the device (its own hub
 * thread does that), and when the interface walk finds a candidate this
 * class driver claims it.  The matching rule is deliberately the driver's
 * own, not a second copy of it:
 *
 *  - VID/PID against the table the urtwn adapter exposes, and
 *  - the interface must carry at least two bulk endpoints (one IN, one
 *    OUT).  The dongles on this board are single-interface vendor-class
 *    devices (0xff/0xff/0xff); the endpoint count is what keeps a composite
 *    dongle's mass-storage or Bluetooth interface from being claimed - the
 *    same test the old harness used after the RTL8821CU modeswitch taught
 *    it why.
 *
 * On match: wlan_usbdi_attach() builds the real NetBSD struct usbd_device
 * over the hubport, and the chip driver's attach runs on it.  Running the
 * attach from the CherryUSB hub thread is intended - it is the same context
 * the NetBSD chain attaches in, and it is the thread whose 64 KiB stack the
 * firmware load and ieee80211_ifattach need.
 *
 * @date   27.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <string.h>

#include "usbh_core.h"
#include "usbh_hub.h"

#include "wlan_cherryusb.h"

/* The chip-side entry: allocated by the NetBSD adapter (urtwn_reg.c), it
 * takes the device the shim built and runs the driver's attach on it. */
extern int wlan_urtwn_cherryusb_attach(struct usbd_device *udev,
    uint16_t vendor, uint16_t product);
extern void wlan_urtwn_cherryusb_detach(struct usbd_device *udev);

/* VID/PID pairs this hook claims, in the shape CherryUSB's class-info table
 * wants (`{vid, pid}` rows, zero-terminated).  Kept beside the driver's own
 * table on purpose: this list is "which devices may reach the driver", the
 * driver's is "which chips it can drive"; the intersection is what attaches. */
static const uint16_t usbh_wlan_ids[][2] = {
	{ 0x0bda, 0x8179 },	/* RTL8188EUS (the line's acceptance card) */
	{ 0x0bda, 0x0179 },	/* RTL8188EU */
	{ 0, 0 }
};

/* How many bulk endpoints the interface must carry for us to consider it a
 * wireless data interface (urtwn opens one IN and one OUT pipe). */
#define USBH_WLAN_MIN_BULK_EPS	2

static int usbh_wlan_id_match(uint16_t vendor, uint16_t product)
{
	uint32_t i;

	for (i = 0; usbh_wlan_ids[i][0] != 0; i++) {
		if (usbh_wlan_ids[i][0] == vendor &&
		    usbh_wlan_ids[i][1] == product) {
			return 1;
		}
	}
	return 0;
}

static uint32_t usbh_wlan_bulk_ep_count(const struct usbh_hubport *hport,
					uint8_t intf, int *has_in, int *has_out)
{
	const struct usbh_interface_altsetting *alt =
		&hport->config.intf[intf].altsetting[0];
	uint32_t count = 0;
	uint8_t i;

	*has_in = 0;
	*has_out = 0;
	for (i = 0; i < CONFIG_USBHOST_MAX_ENDPOINTS; i++) {
		uint8_t attr = alt->ep[i].ep_desc.bmAttributes;

		if (alt->ep[i].ep_desc.bLength == 0) {
			continue;
		}
		if (USB_GET_ENDPOINT_TYPE(attr) != USB_ENDPOINT_TYPE_BULK) {
			continue;
		}
		count++;
		if (alt->ep[i].ep_desc.bEndpointAddress & 0x80U) {
			*has_in = 1;
		} else {
			*has_out = 1;
		}
	}
	return count;
}

static int usbh_wlan_connect(struct usbh_hubport *hport, uint8_t intf)
{
	struct usbd_device *udev;
	int has_in = 0;
	int has_out = 0;

	if (!usbh_wlan_id_match(hport->device_desc.idVendor,
				hport->device_desc.idProduct)) {
		return -USB_ERR_NODEV;
	}
	if (usbh_wlan_bulk_ep_count(hport, intf, &has_in, &has_out) <
	    USBH_WLAN_MIN_BULK_EPS || !has_in || !has_out) {
		/* Some other interface of a composite device (or a dongle in
		 * its pre-modeswitch person): not ours. */
		return -USB_ERR_NODEV;
	}

	udev = wlan_usbdi_attach(hport, NULL);
	if (udev == NULL) {
		USB_LOG_ERR("wlan: usbdi attach failed for %04x:%04x\r\n",
			    hport->device_desc.idVendor,
			    hport->device_desc.idProduct);
		return -USB_ERR_NOMEM;
	}

	/* Remember the shim's device on the interface: the disconnect hook
	 * runs before the hubport is torn down and has this array. */
	hport->config.intf[intf].priv = udev;

	if (wlan_urtwn_cherryusb_attach(udev, hport->device_desc.idVendor,
					hport->device_desc.idProduct) != 0) {
		hport->config.intf[intf].priv = NULL;
		wlan_usbdi_detach(udev);
		return -USB_ERR_INVAL;
	}

	return 0;
}

static void usbh_wlan_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
	struct usbd_device *udev = hport->config.intf[intf].priv;

	if (udev == NULL) {
		return;
	}
	hport->config.intf[intf].priv = NULL;
	wlan_urtwn_cherryusb_detach(udev);
	wlan_usbdi_detach(udev);
}

static const struct usbh_class_driver usbh_wlan_class_driver = {
	.driver_name = "wlan80211",
	.driver_desc = "IEEE 802.11 wireless dongles (NetBSD usbdi driver on the compat layer)",
	.connect = usbh_wlan_connect,
	.disconnect = usbh_wlan_disconnect,
};

/* CLASS_INFO_DEFINE puts this in the .usbh_class_info section, which the
 * linker script KEEPs and usbh_initialize() brackets - without the KEEP the
 * linker drops the entry (nothing references it) and no dongle is claimed. */
CLASS_INFO_DEFINE const struct usbh_class_info usbh_wlan_class_info = {
	.match_flags = USB_CLASS_MATCH_VID_PID,
	.id_table = usbh_wlan_ids,
	.bInterfaceClass = 0xff,
	.bInterfaceSubClass = 0xff,
	.bInterfaceProtocol = 0xff,
	.class_driver = &usbh_wlan_class_driver,
};
