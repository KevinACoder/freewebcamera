/*
 * @file
 * @brief `usbreq` shell command: drive the usb host abstraction (include/
 *        usb_host.h) from the console.
 *
 * This is the abstraction's in-tree consumer, and its reason to exist is the
 * port line's stated question: what does a USB request actually look like
 * from above the stack?  Every other caller in this image reaches through
 * the imported usbdi world directly, so without a first-party consumer the
 * seam would be untested code sitting between two things that both work.  A
 * request issued from here goes: shell -> usb_host.h -> backend -> usbdi ->
 * EHCI/xHCI, and the answer comes back the same way.
 *
 * Verbs:
 *   usbreq list                     - enumerate devices (index, addr, speed,
 *                                     VID:PID, class)
 *   usbreq info <idx>               - one device's snapshot
 *   usbreq desc <idx> [len]         - GET_DESCRIPTOR(device) through the
 *                                     abstraction's control-transfer call
 *   usbreq ctrl <idx> <bm> <req> <wValue> <wIndex> <len>
 *                                   - an arbitrary setup packet (the escape
 *                                     hatch: probe a device's own requests)
 *
 * All numbers accept 0x-prefixed hex or decimal.  Same shape as the wlan/uvc
 * commands: dispatch in the command body, output through csh_printf.
 *
 * @date 28.09.2026
 * @author zhugengyu
 */

#include <stdint.h>
#include <string.h>

#include "cherrysh_adapter.h"
#include "csh.h"
#include "usb_host.h"

/* Freestanding: minilibc.c provides the definition (same extern the shell
 * adapter uses for its own parsing). */
extern int atoi(const char *s);

extern ARM_DRIVER_USB_HOST Driver_USB_HOST_NetBSD;

static const char *usbreq_speed_str(uint8_t speed)
{
	switch (speed) {
	case USB_HOST_SPEED_LOW:
		return "low";
	case USB_HOST_SPEED_FULL:
		return "full";
	case USB_HOST_SPEED_HIGH:
		return "high";
	case USB_HOST_SPEED_SUPER:
		return "super";
	default:
		return "?";
	}
}

/* "0x1d6b" / "7531"; negative on garbage. */
static int usbreq_num(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	int base = 10;

	if (s == NULL || *s == '\0') {
		return -1;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (; *s != '\0'; s++) {
		uint32_t d;

		if (*s >= '0' && *s <= '9') {
			d = (uint32_t) (*s - '0');
		} else if (base == 16 && *s >= 'a' && *s <= 'f') {
			d = (uint32_t) (*s - 'a' + 10);
		} else if (base == 16 && *s >= 'A' && *s <= 'F') {
			d = (uint32_t) (*s - 'A' + 10);
		} else {
			return -1;
		}
		v = (v * (uint32_t) base) + d;
	}
	*out = v;
	return 0;
}

static int usbreq_list(chry_shell_t *csh)
{
	uint32_t count = 0;
	uint32_t i;
	int32_t ret;

	ret = Driver_USB_HOST_NetBSD.GetDeviceCount(&count);
	if (ret != USB_HOST_OK) {
		csh_printf(csh, "usbreq: device count failed (%d)\n", (int) ret);
		return 1;
	}
	csh_printf(csh, "usbreq: %u device(s)\n", (unsigned) count);
	for (i = 0; i < count; i++) {
		USB_HOST_DEVICE info;

		if (Driver_USB_HOST_NetBSD.GetDeviceInfo(i, &info) != USB_HOST_OK) {
			continue;
		}
		csh_printf(csh,
			   "  [%u] addr %u %s vid:pid %04x:%04x class %02x cfg %u\n",
			   (unsigned) info.index, (unsigned) info.address,
			   usbreq_speed_str(info.speed), (unsigned) info.vendor,
			   (unsigned) info.product, (unsigned) info.device_class,
			   (unsigned) info.num_configurations);
	}
	return 0;
}

static int usbreq_info(chry_shell_t *csh, const char *arg)
{
	USB_HOST_DEVICE info;
	uint32_t index;
	int32_t ret;

	if (usbreq_num(arg, &index) != 0) {
		csh_printf(csh, "usbreq: bad index '%s'\n", arg != NULL ? arg : "");
		return 1;
	}
	ret = Driver_USB_HOST_NetBSD.GetDeviceInfo(index, &info);
	if (ret != USB_HOST_OK) {
		csh_printf(csh, "usbreq: no device at index %u (%d)\n",
			   (unsigned) index, (int) ret);
		return 1;
	}
	csh_printf(csh,
		   "usbreq: [%u] addr %u speed %s class %02x vid:pid %04x:%04x "
		   "bcd %04x configs %u configured %u\n",
		   (unsigned) info.index, (unsigned) info.address,
		   usbreq_speed_str(info.speed), (unsigned) info.device_class,
		   (unsigned) info.vendor, (unsigned) info.product,
		   (unsigned) info.bcd_device, (unsigned) info.num_configurations,
		   info.configured ? 1U : 0U);
	return 0;
}

/* GET_DESCRIPTOR(device) - the canonical request, and the first thing any
 * stack port has to get right: a fixed 8-byte first read, then the full
 * descriptor the device announced.  Printing both lengths is the point: the
 * second only works if the first was parsed. */
static int usbreq_desc(chry_shell_t *csh, const char *idxarg, const char *lenarg)
{
	USB_HOST_REQUEST req;
	uint32_t index;
	uint32_t want = 18;
	uint32_t got = 0;
	uint8_t buf[64];
	int32_t ret;
	unsigned i;

	if (usbreq_num(idxarg, &index) != 0) {
		csh_printf(csh, "usbreq: bad index\n");
		return 1;
	}
	if (lenarg != NULL) {
		if (usbreq_num(lenarg, &want) != 0 || want == 0) {
			csh_printf(csh, "usbreq: bad length\n");
			return 1;
		}
	}
	if (want > sizeof(buf)) {
		want = sizeof(buf);
	}

	memset(&req, 0, sizeof(req));
	req.bmRequestType = 0x80;	/* device-to-host, standard, device */
	req.bRequest = 6;		/* GET_DESCRIPTOR */
	req.wValue = 0x0100;		/* descriptor type 1 (device), index 0 */
	req.wIndex = 0;
	req.wLength = (uint16_t) want;

	ret = Driver_USB_HOST_NetBSD.ControlTransfer(index, &req, buf, &got,
	    USB_HOST_DEFAULT_TIMEOUT_MS);
	if (ret != USB_HOST_OK && ret != USB_HOST_ERROR_SHORT) {
		csh_printf(csh, "usbreq: GET_DESCRIPTOR failed (%d)\n", (int) ret);
		return 1;
	}
	csh_printf(csh, "usbreq: descriptor, %u byte(s)%s\n", (unsigned) got,
		   (ret == USB_HOST_ERROR_SHORT) ? " (short)" : "");
	for (i = 0; i < got; i++) {
		csh_printf(csh, "%02x%s", (unsigned) buf[i],
			   ((i & 15) == 15 || i + 1 == got) ? "\n" : " ");
	}
	return 0;
}

static int usbreq_ctrl(chry_shell_t *csh, int argc, char **argv)
{
	USB_HOST_REQUEST req;
	uint32_t index, bm, br, wval, widx, len;
	uint32_t got = 0;
	uint8_t buf[256];
	int32_t ret;
	unsigned i;

	if (argc < 7) {
		csh_printf(csh,
			   "usage: usbreq ctrl <idx> <bmRequestType> <bRequest> "
			   "<wValue> <wIndex> <len>\n");
		return 1;
	}
	if (usbreq_num(argv[2], &index) != 0 || usbreq_num(argv[3], &bm) != 0 ||
	    usbreq_num(argv[4], &br) != 0 || usbreq_num(argv[5], &wval) != 0 ||
	    usbreq_num(argv[6], &widx) != 0 || usbreq_num(argv[7], &len) != 0) {
		csh_printf(csh, "usbreq: bad argument (numbers take 0x.. or decimal)\n");
		return 1;
	}
	if (len > sizeof(buf)) {
		csh_printf(csh, "usbreq: len capped at %u\n", (unsigned) sizeof(buf));
		len = sizeof(buf);
	}

	memset(&req, 0, sizeof(req));
	req.bmRequestType = (uint8_t) bm;
	req.bRequest = (uint8_t) br;
	req.wValue = (uint16_t) wval;
	req.wIndex = (uint16_t) widx;
	req.wLength = (uint16_t) len;

	ret = Driver_USB_HOST_NetBSD.ControlTransfer(index, &req, buf, &got,
	    USB_HOST_DEFAULT_TIMEOUT_MS);
	csh_printf(csh, "usbreq: ctrl -> %d, %u byte(s)\n", (int) ret,
		   (unsigned) got);
	if (ret != USB_HOST_OK && ret != USB_HOST_ERROR_SHORT) {
		return 1;
	}
	/* IN transfers print their payload; an OUT request's buffer is
	 * caller garbage and has nothing to show. */
	if ((bm & 0x80) != 0) {
		for (i = 0; i < got; i++) {
			csh_printf(csh, "%02x%s", (unsigned) buf[i],
				   ((i & 15) == 15 || i + 1 == got) ? "\n" : " ");
		}
	}
	return 0;
}

static int cmd_usbreq(int argc, char **argv)
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	const char *verb = (argc >= 2) ? argv[1] : "list";

	if (strcmp(verb, "list") == 0) {
		return usbreq_list(csh);
	}
	if (strcmp(verb, "info") == 0) {
		return usbreq_info(csh, (argc >= 3) ? argv[2] : NULL);
	}
	if (strcmp(verb, "desc") == 0) {
		return usbreq_desc(csh, (argc >= 3) ? argv[2] : NULL,
				   (argc >= 4) ? argv[3] : NULL);
	}
	if (strcmp(verb, "ctrl") == 0) {
		return usbreq_ctrl(csh, argc, argv);
	}
	csh_printf(csh,
		   "usage: usbreq list | info <idx> | desc <idx> [len] | "
		   "ctrl <idx> <bm> <req> <wValue> <wIndex> <len>\n");
	return 1;
}

CSH_CMD_EXPORT_ALIAS_FULL(cmd_usbreq, usbreq, "usbreq",
			  "usb host requests through the abstraction: "
			  "list | info | desc | ctrl");
