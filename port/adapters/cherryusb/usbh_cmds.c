/*
 * @file   usbh_cmds.c
 * @brief  Shell command for the USB host: the operational view the
 *         acceptance runs are written against.
 *
 *   usbh               device tree of every bus (one line per device)
 *   usbh list [-t]     same, -t adds the hub/port hierarchy
 *   usbh start         (re)start both buses - idempotent after boot
 *
 * The two panel wireless NICs show up here as enumerated devices without a
 * claimed interface (RTL8188EUS and AIC8800D80 behind the onboard CH334P
 * hub); driving them is explicitly out of scope for this milestone.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#include <stdint.h>
#include <string.h>

#include "cherrysh_adapter.h"
#include "csh.h"

#include "usb.h"
#include "usbh_core.h"
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
	default:
		return "?";
	}
}

static const char *usbh_indent(uint8_t depth)
{
	static const char pad[] = "                    ";

	return pad + sizeof(pad) - 1U - (uint32_t)depth * 2U;
}

static void usbh_tree_print(chry_shell_t *csh, struct usbh_hub *hub,
			    uint8_t depth)
{
	uint8_t port;

	for (port = 0; port < hub->nports; port++) {
		struct usbh_hubport *hport = &hub->child[port];

		if (!hport->connected) {
			continue;
		}

		csh_printf(csh, "%sport %u: %04x:%04x %s %s%s\r\n",
			   usbh_indent(depth), (uint32_t)port + 1U,
			   hport->device_desc.idVendor,
			   hport->device_desc.idProduct,
			   usbh_speed_str(hport->speed),
			   (hport->device_desc.bDeviceClass == 0x09U) ? "hub" :
								       "dev",
			   (hport->self != NULL) ? " (hub)" : "");

		if (hport->self != NULL) {
			usbh_tree_print(csh, hport->self, (uint8_t)(depth + 1U));
		}
	}
}

static void usbh_list_print(chry_shell_t *csh, bool tree)
{
	uint8_t busid;

	for (busid = 0; busid < USBH_EHCI_NUM; busid++) {
		struct usbh_bus *bus = &g_usbhost_bus[busid];
		struct usbh_hub *roothub = &bus->hcd.roothub;

		if (roothub->int_buffer == NULL && roothub->nports == 0U) {
			csh_printf(csh, "bus%u: not started\r\n", busid);
			continue;
		}

		csh_printf(csh, "bus%u: ehci @%08x\r\n", busid,
			   (uint32_t)USBH_EHCI_BASE(busid));
		if (tree) {
			usbh_tree_print(csh, roothub, 1U);
		} else {
			usbh_tree_print(csh, roothub, 0U);
		}
	}
}

static int cmd_usbh(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);

	if (argc >= 2 && strcmp(argv[1], "start") == 0) {
		if (usb_start() != 0) {
			csh_printf(csh, "usbh: start failed (see log)\r\n");
			return -1;
		}
		csh_printf(csh, "usbh: started\r\n");
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

	csh_printf(csh, "usage: usbh [list [-t]|start]\r\n");
	return -1;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_usbh, usbh, "usbh [list [-t]|start]",
			  "show the usb host device tree or start the buses");
