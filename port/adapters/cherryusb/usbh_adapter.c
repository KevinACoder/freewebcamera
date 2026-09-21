/*
 * @file   usbh_adapter.c
 * @brief  CherryUSB host-stack adapter: console logging, enumeration event
 *         reporting and the boot-time start path for both EHCI buses.
 *
 * The start path carries the one board-specific asymmetry of this port:
 * KI-006. The panel hubs sit plugged in before the image boots, so after
 * HCRESET the root ports read CCS=1 without ever raising a connect-change
 * edge, and the hub thread would sleep forever waiting for one. After the
 * controller init settles, ports with a device get a power-off/power-on
 * kick to manufacture that edge, the roothub change bits are seeded, and
 * the hub thread is woken - the same sequence the standalone line verified.
 *
 * usbh_initialize() starts a hub thread per bus which performs usb_hc_init()
 * internally, so the caller cannot assume the registers are live when it
 * returns; the wait below polls the controller instead of guessing.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "board.h"

#include "usb.h"
#include "usbh_core.h"
#include "usbh_hub.h"
#include "usb_hc_ehci.h"
#ifdef CONFIG_USBHOST_MULTI_HCD
#include "xhci/usbh_xhci_glue.h"
#include "xhci/usb_hc_xhci.h"
#endif
#include "usb_board.h"

/* --- console -------------------------------------------------------------- */

/* Single buffer + critical section: the formatter runs from both the hub
 * thread and (on error paths) ISR context, and interleaved half-lines are
 * worse than a short lock. */
void usbh_console_printf(const char *fmt, ...)
{
	static char buf[192];
	size_t flags;
	va_list ap;

	flags = usb_osal_enter_critical_section();
	va_start(ap, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	board_early_print(buf);
	usb_osal_leave_critical_section(flags);
}

/* --- enumeration event report ---------------------------------------------- */

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

/* Safe port lookup by (hub index, port): the vendored usbh_find_hubport
 * asserts hub_index <= roothub->index (1), so it cannot address a device
 * behind the panel's external hub. Walk the tree ourselves and return NULL
 * instead of parking the system when the lookup has no answer. */
static struct usbh_hubport *usbh_find_port_safe(struct usbh_bus *bus,
						uint8_t hub_index,
						uint8_t hub_port)
{
	struct usbh_hub *stack[CONFIG_USBHOST_MAX_EXTHUBS + 1U];
	int top = 0;

	stack[top++] = &bus->hcd.roothub;

	while (top > 0) {
		struct usbh_hub *hub = stack[--top];
		uint8_t port;

		if (hub->index == hub_index) {
			struct usbh_hubport *hport;

			if ((hub_port < 1U) || (hub_port > hub->nports)) {
				return NULL;
			}
			hport = &hub->child[hub_port - 1U];
			return hport->connected ? hport : NULL;
		}

		for (port = 0U; port < hub->nports; port++) {
			if (hub->child[port].connected &&
			    (hub->child[port].self != NULL) &&
			    (top < (int)(sizeof(stack) / sizeof(stack[0])))) {
				stack[top++] = hub->child[port].self;
			}
		}
	}

	return NULL;
}

static void usbh_report_port(struct usbh_bus *bus, uint8_t hub_index,
			     uint8_t hub_port, const char *what)
{
	struct usbh_hubport *hport = usbh_find_port_safe(bus, hub_index,
							 hub_port);

	if (hport != NULL) {
		usbh_console_printf("usbh: bus%u hub%u port%u %s %04x:%04x %s"
				    " (%s)\r\n",
				    bus->busid, hub_index, hub_port, what,
				    hport->device_desc.idVendor,
				    hport->device_desc.idProduct,
				    hport->device_desc.bDeviceClass == 0x09U ?
				    "hub" : "device",
				    usbh_speed_str(hport->speed));
	} else {
		usbh_console_printf("usbh: bus%u hub%u port%u %s\r\n",
				    bus->busid, hub_index, hub_port, what);
	}
}

static void usbh_bus_event(uint8_t busid, uint8_t hub_index, uint8_t hub_port,
			   uint8_t intf, uint8_t event)
{
	struct usbh_bus *bus = &g_usbhost_bus[busid];

	(void)intf;

	switch (event) {
	case USBH_EVENT_INIT:
		usbh_console_printf("usbh: bus%u hc init\r\n", busid);
		break;
	case USBH_EVENT_INTERFACE_START:
		usbh_report_port(bus, hub_index, hub_port, "claimed");
		break;
	case USBH_EVENT_INTERFACE_UNSUPPORTED:
		/* The two panel wireless NICs land here: enumerated, no
		 * driver - which is exactly this milestone's scope. */
		usbh_report_port(bus, hub_index, hub_port, "no-driver");
		break;
	case USBH_EVENT_DEVICE_DISCONNECTED:
		usbh_report_port(bus, hub_index, hub_port, "gone");
		break;
	default:
		break;
	}
}

/* --- start path ------------------------------------------------------------ */

/* KI-006: manufacture the connect-change edge for pre-plugged root ports,
 * seed the roothub change bits, and wake the hub thread (which is blocked
 * on its queue with nothing pending right after usb_hc_init). */
static void usbh_ehci_post_init(struct usbh_bus *bus)
{
	struct ehci_hcd *hcd = &g_ehci_hcd[bus->hcd.hcd_id];
	uint8_t port;

	for (port = 1U; port <= hcd->n_ports; port++) {
		uint32_t regval = EHCI_HCOR->portsc[port - 1U];

		usbh_console_printf(
			"usbh: bus%u port%u portsc=%08x ppc=%u\r\n",
			bus->busid, port, regval, hcd->ppc);

		if (hcd->ppc && (regval & EHCI_PORTSC_CCS) != 0U) {
			regval &= ~EHCI_PORTSC_PP;
			EHCI_HCOR->portsc[port - 1U] = regval;
			usb_osal_msleep(30);
			regval |= EHCI_PORTSC_PP;
			EHCI_HCOR->portsc[port - 1U] = regval;
			usb_osal_msleep(30);
			usbh_console_printf(
				"usbh: bus%u port%u kicked, now %08x\r\n",
				bus->busid, port,
				EHCI_HCOR->portsc[port - 1U]);
		}

		bus->hcd.roothub.int_buffer[port / 8U] |=
			(uint8_t)(1U << (port % 8U));
	}

	usbh_console_printf("usbh: bus%u wake hub thread (intbuf=%02x%02x)\r\n",
			    bus->busid,
			    bus->hcd.roothub.int_buffer[1],
			    bus->hcd.roothub.int_buffer[0]);
	usbh_hub_thread_wakeup(&bus->hcd.roothub);
}

#ifdef CONFIG_USBHOST_MULTI_HCD
/* xHCI flavour: no KI-006 power-cycle kick (the driver's own init already
 * powers the ports with the read-back retry), just seed the roothub change
 * bits for ports that already carry a device and wake the hub thread. The
 * real root port count was published by the glue's low-level init, so the
 * thread enumerates exactly the live ports. */
static void usbh_xhci_post_init(struct usbh_bus *bus)
{
	uint8_t port;
	uint8_t nports = usbh_xhci_nports(bus->busid);

	for (port = 1U; port <= nports; port++) {
		uint32_t ps = usbh_xhci_portsc(bus->busid, port);

		usbh_console_printf("usbh: bus%u port%u portsc=%08x\r\n",
				    bus->busid, port, ps);

		if ((ps & XHCI_PS_CCS) != 0U) {
			bus->hcd.roothub.int_buffer[port / 8U] |=
				(uint8_t)(1U << (port % 8U));
		}
	}

	usbh_console_printf("usbh: bus%u wake hub thread (intbuf=%02x%02x)\r\n",
			    bus->busid,
			    bus->hcd.roothub.int_buffer[1],
			    bus->hcd.roothub.int_buffer[0]);
	usbh_hub_thread_wakeup(&bus->hcd.roothub);
}

/* Hot-plug watchdog (task side, per the D36 division: the ISR only records
 * events, waking stays a task-context operation). Polls each xHCI instance's
 * port event sequence - NOT the roothub_intbuf, which the stack never clears
 * and whose leftovers would re-trigger enumeration forever - and wakes the
 * hub thread whenever a new Port Status Change event has arrived since the
 * last poll. Runs at BelowNormal so it never competes with real work. */
static void usbh_xhci_hotplug_watchdog(void)
{
	static uint32_t last_seq[USBH_XHCI_NUM];
	static bool seeded;
	uint8_t i;

	for (i = 0U; i < USBH_XHCI_NUM; i++) {
		uint8_t busid = (uint8_t)(USBH_XHCI0_BUSID + i);
		uint32_t seq = usbh_xhci_port_evt_seq(busid);

		if (!seeded) {
			/* First pass: events up to here were already handed to
			 * the hub thread by usbh_xhci_post_init. */
			last_seq[i] = seq;
		} else if (seq != last_seq[i]) {
			last_seq[i] = seq;
			usbh_hub_thread_wakeup(&g_usbhost_bus[busid].hcd.roothub);
		}
	}
	seeded = true;
}
#endif /* CONFIG_USBHOST_MULTI_HCD */

/* One bus: initialize the stack (and register the bus's HCD ops before the
 * hub thread can run usb_hc_init), wait for that init to reach the end, then
 * run the pre-plugged-device seed. Returns 0 when the controller is live. */
#ifdef CONFIG_USBHOST_MULTI_HCD
static int usbh_bus_start(uint8_t busid)
{
	struct usbh_bus *bus = &g_usbhost_bus[busid];
	uint32_t i;

	if (USBH_BUS_IS_EHCI(busid)) {
		struct ehci_hcd *hcd = &g_ehci_hcd[busid];

		if (usbh_initialize(busid, USBH_EHCI_BASE(busid),
				    usbh_bus_event) != 0) {
			return -1;
		}
		usbh_hcd_register(busid, &usbh_ehci_ops);

		/* hcor_offset is read from the capability block mid-init; only
		 * then does an HCOR access (usbintr below) hit the operational
		 * frame. */
		for (i = 0U; i < 300U; i++) {
			if (hcd->hcor_offset != 0U) {
				break;
			}
			usb_osal_msleep(10);
		}
		for (i = 0U; i < 300U; i++) {
			if (EHCI_HCOR->usbintr != 0U) {
				break;
			}
			usb_osal_msleep(10);
		}
		if (EHCI_HCOR->usbintr == 0U) {
			usbh_console_printf("usbh: bus%u hc init timeout\r\n",
					    busid);
			return -1;
		}

		usbh_ehci_post_init(bus);
		return 0;
	}

	if (USBH_XHCI_INST(busid) >= USBH_XHCI_NUM) {
		return -1;
	}
	if (usbh_initialize(busid, USBH_XHCI_BASE(busid), usbh_bus_event) != 0) {
		return -1;
	}
	usbh_hcd_register(busid, &usbh_xhci_ops);

	/* The glue gates register access on its own "MMIO alive" flag, so
	 * this poll is safe from the first iteration on; the controller
	 * flips USBCMD.RS / clears HCH as the last init step. */
	for (i = 0U; i < 300U; i++) {
		if (usbh_xhci_hc_running(busid)) {
			break;
		}
		usb_osal_msleep(10);
	}
	if (!usbh_xhci_hc_running(busid)) {
		usbh_console_printf("usbh: bus%u hc init timeout\r\n", busid);
		return -1;
	}

	usbh_xhci_post_init(bus);
	return 0;
}
#else
static int usbh_bus_start(uint8_t busid)
{
	struct usbh_bus *bus = &g_usbhost_bus[busid];
	struct ehci_hcd *hcd = &g_ehci_hcd[busid];
	uint32_t i;

	if ((int)busid >= (int)USBH_EHCI_NUM) {
		return -1;
	}

	if (usbh_initialize(busid, USBH_EHCI_BASE(busid), usbh_bus_event) != 0) {
		return -1;
	}

	/* hcor_offset is read from the capability block mid-init; only then
	 * does an HCOR access (usbintr below) hit the operational frame. */
	for (i = 0U; i < 300U; i++) {
		if (hcd->hcor_offset != 0U) {
			break;
		}
		usb_osal_msleep(10);
	}
	for (i = 0U; i < 300U; i++) {
		if (EHCI_HCOR->usbintr != 0U) {
			break;
		}
		usb_osal_msleep(10);
	}
	if (EHCI_HCOR->usbintr == 0U) {
		usbh_console_printf("usbh: bus%u hc init timeout\r\n", busid);
		return -1;
	}

	usbh_ehci_post_init(bus);
	return 0;
}
#endif

/* Bring up every compiled-in bus. Idempotent; returns 0 when all started. */
int usb_start(void)
{
	static bool started;
	uint8_t busid;
	int fails = 0;

	if (started) {
		return 0;
	}
	started = true;

#ifdef THREADX_BUILD
	/* The ThreadX osal carries its own byte pool and self-delete reaper;
	 * they must exist before the first usb_osal_* allocation below. The
	 * FreeRTOS osal has no init step. Same kernel-boundary ifdef as the
	 * banner in main.c. Upstream declares no prototype for this - it is
	 * the integrator's side of the osal. */
	extern void usb_osal_init(uint8_t *mem, uint32_t mem_size);

	usb_osal_init(NULL, 0U);
#endif

	for (busid = 0U; busid < (USBH_EHCI_NUM + USBH_XHCI_NUM); busid++) {
		if (usbh_bus_start(busid) != 0) {
			usbh_console_printf("usbh: bus%u start FAIL\r\n",
					    busid);
			fails++;
		}
	}

#ifdef CONFIG_USBHOST_MULTI_HCD
	if (fails == 0) {
		/* Stay resident as the hot-plug watchdog: the xHCI ISR only
		 * records port events, so without this loop a device plugged
		 * in after boot would never reach the hub thread. The EHCI
		 * ISRs wake the hub thread directly. */
		for (;;) {
			usbh_xhci_hotplug_watchdog();
			usb_osal_msleep(100U);
		}
	}
#endif

	return (fails != 0) ? -1 : 0;
}
