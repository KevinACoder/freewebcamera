/*
 * @file
 * @brief freewebcamera adapter of the NetBSD net80211 + usb stack.
 *
 * Binds the imported stack to this image's CMSIS-RTOS2 surface: the
 * heap binding behind the OSAL's weak allocation hooks, the worker
 * thread primitive, the firmware registry, and the lifecycle entry
 * that brings the USB platform + the NetBSD usbus chain up (the
 * config_interrupts hooks then run on the config worker, so the
 * attach chain blocks its own thread, not the shell).
 *
 * @author zhugengyu
 * @date 25.09.2026
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

#include "board.h"

#include "port.h"
#include "wlan_port_cmsis.h"
#include "usb_platform.h"

/* ------------------------------------------------------------------ */
/* heap binding: overrides the OSAL's weak malloc/free defaults */

extern void *pvPortMalloc(size_t length);
extern void vPortFree(void *ptr);

void *wlan_osal_alloc(size_t size) {
	return pvPortMalloc(size);
}

void wlan_osal_free(void *p) {
	vPortFree(p);
}

/* ------------------------------------------------------------------ */
/* worker threads (kthread/softint/config backends all land here).
 *
 * Priority: the workers sit BELOW the (future) TCP/IP thread - the D54
 * lesson from the net_80211 line: a driver work segment that preempts
 * tcpip mid-burst fills its mailbox. osPriorityBelowNormal maps under
 * osPriorityNormal on this CMSIS twin. */

void *wlan_port_thread_create(void (*run)(void *), void *arg) {
	static const osThreadAttr_t attr = {
		.name = "wlan-work",
		.priority = osPriorityBelowNormal,
		.stack_size = 4096,
	};

	return (void *) osThreadNew((osThreadFunc_t) run, arg, &attr);
}

/* ------------------------------------------------------------------ */
/* firmware: the blobs embedded at build time (generated from the
 * net80211 submodule's realtek dist by tools/gen_firmware_array.py) */

extern const uint8_t rtl8188eufw_data[];
extern const size_t rtl8188eufw_size;

/* ------------------------------------------------------------------ */

static int wlan_started;

void wlan_console_ready(void);

int wlan_adapter_ready(void) {
	return wlan_started;
}

int wlan_start(void) {
	if (wlan_started) {
		return 0;
	}
	wlan_osal_cmsis_init();
	wlan_console_ready();
		/* the netbsd usb history log level for the bring-up rounds
		 * (ehci's level comes from EHCI_DEBUG_DEFAULT) */
		{
			extern int usbdebug;

			usbdebug = 10;
		}
	if (wlan_port_firmware_register("rtl8188eufw.bin", rtl8188eufw_data,
		(size_t) rtl8188eufw_size) != 0) {
		printf("wlan: firmware registration failed\n");
		return -1;
	}

	/* platform power-up + ehci_init + config_found: the enumeration,
	 * hub exploration and urtwn attach (with its firmware load) run
	 * on the calling thread and the threads the chain spawns. */
	if (usb_platform_init() != 0) {
		printf("wlan: usb platform init failed\n");
		return -1;
	}
	wlan_started = 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* port core: the bus-neutral shell the shell commands dispatch onto
 * (see port.h). One adapter, one active device: the registry keeps the
 * first adapter that registered and every port.* entry point lands on
 * it. */

static const struct wlan_port_adapter *wlan_active;

void wlan_port_adapter_register(const struct wlan_port_adapter *adapter) {
	if (wlan_active == NULL) {
		wlan_active = adapter;
		printf("wlan: adapter '%s' registered\n", adapter->name);
	}
}

int wlan_port_select(const char *name) {
	if (wlan_active == NULL || name == NULL) {
		return -1;
	}
	return strcmp(wlan_active->name, name) == 0 ? 0 : -1;
}

const char *wlan_port_active_name(void) {
	return wlan_active != NULL ? wlan_active->name : NULL;
}

void *wlan_port_get_ic(void) {
	return wlan_active != NULL ? wlan_active->ic : NULL;
}

int wlan_port_up(void) {
	if (wlan_active == NULL || wlan_active->up == NULL) {
		return -1;
	}
	return wlan_active->up();
}

int wlan_port_scan(const uint8_t *ssid, size_t len) {
	if (wlan_active == NULL || wlan_active->scan == NULL) {
		return -1;
	}
	return wlan_active->scan(ssid, len);
}

int wlan_port_xmit(const uint8_t *frame, size_t len) {
	if (wlan_active == NULL || wlan_active->xmit == NULL) {
		return -1;
	}
	return wlan_active->xmit(frame, len);
}

int wlan_port_get_hwaddr(uint8_t addr[6]) {
	if (wlan_active == NULL || wlan_active->get_hwaddr == NULL) {
		return -1;
	}
	return wlan_active->get_hwaddr(addr);
}

void wlan_port_status_dump(void) {
	if (wlan_active == NULL || wlan_active->status_dump == NULL) {
		printf("wlan: no adapter\n");
		return;
	}
	wlan_active->status_dump();
}

void wlan_port_scan_dump(void) {
	if (wlan_active == NULL || wlan_active->scan_dump == NULL) {
		printf("wlan: no adapter\n");
		return;
	}
	wlan_active->scan_dump();
}

void wlan_port_deinit(void) {
}
