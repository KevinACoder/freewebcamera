/*
 * @file   usbh_cmds.c
 * @brief  Shell command for the CherryUSB host stack: the operational view
 *         the acceptance runs are written against, plus the independent
 *         start entry (the platform is not tied to `wlan start`).
 *
 *   usbh               device tree of every bus (one line per device)
 *   usbh list [-t]     same, -t adds the hub/port hierarchy
 *   usbh start         bring the buses up - idempotent, also runs from
 *                      `wlan start`'s platform hook
 *
 * Devices with no matching class driver (the panel's RTL8188EUS until the
 * urtwn class hook lands, the AIC8800D80, a UVC camera without the video
 * class) show up here as enumerated and reported; driving them is the
 * next line's job.
 *
 * @date   27.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <string.h>

#include "cherrysh_adapter.h"
#include "csh.h"

#include "usbh_core.h"
#include "usbh_platform.h"
#include "usb_board.h"

static const char *usbh_speed_str(uint8_t speed)
{
	switch (speed) {
	case USB_SPEED_LOW:
		return "low";
	case USB_SPEED_FULL:
		return "full";
	case USB_SPEED_HIGH:
		return "high";
	case USB_SPEED_SUPER:
		return "super";
	default:
		return "?";
	}
}

static void usbh_tree_print(chry_shell_t *csh, struct usbh_hub *hub,
			    uint8_t depth)
{
	uint8_t port;

	for (port = 0; port < hub->nports; port++) {
		struct usbh_hubport *hport = &hub->child[port];
		uint8_t i;

		if (!hport->connected) {
			continue;
		}

		for (i = 0; i < depth; i++) {
			csh_printf(csh, "  ");
		}
		csh_printf(csh, "port %u: %04x:%04x %s %s%s\n",
			   (uint32_t)port + 1U, hport->device_desc.idVendor,
			   hport->device_desc.idProduct,
			   usbh_speed_str(hport->speed),
			   (hport->device_desc.bDeviceClass == 0x09U) ?
			   "hub" : "dev",
			   (hport->self != NULL) ? " (hub)" : "");

		if (hport->self != NULL) {
			usbh_tree_print(csh, hport->self,
					(uint8_t)(depth + 1U));
		}
	}
}

static void usbh_list_print(chry_shell_t *csh, bool tree)
{
	uint8_t busid;

	for (busid = 0U; busid < USBH_EHCI_NUM; busid++) {
		struct usbh_bus *bus = &g_usbhost_bus[busid];
		struct usbh_hub *roothub = &bus->hcd.roothub;
		const char *drv = (bus->hc_driver != NULL) ?
			bus->hc_driver->driver_name : "?";

		if (roothub->int_buffer == NULL && roothub->nports == 0U) {
			csh_printf(csh, "bus%u: not started\n", busid);
			continue;
		}

		csh_printf(csh, "bus%u: %s @%08x\n", busid, drv,
			   (uint32_t)bus->hcd.reg_base);
		usbh_tree_print(csh, roothub, tree ? 1U : 0U);
	}
}

static int cmd_usbh(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	if (argc >= 2 && strcmp(argv[1], "start") == 0) {
		if (usbh_platform_start() != 0) {
			csh_printf(csh, "usbh: start failed (see log)\n");
			return 1;
		}
		csh_printf(csh, "usbh: started\n");
		return 0;
	}

	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		usbh_list_print(csh, (argc >= 3 && strcmp(argv[2], "-t") == 0));
		return 0;
	}

	if (argc == 1) {
		usbh_list_print(csh, false);
		return 0;
	}

	csh_printf(csh, "usage: usbh [list [-t]|start]\n");
	return 1;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_usbh, usbh, "usbh [list [-t]|start]",
			  "show the usb host device tree or start the buses");
