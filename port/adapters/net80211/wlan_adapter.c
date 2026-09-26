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
		.stack_size = 8192,
	};

	return (void *) osThreadNew((osThreadFunc_t) run, arg, &attr);
}

/* ------------------------------------------------------------------ */
/* firmware: the blobs embedded at build time (generated from the
 * net80211 submodule's realtek dist by tools/gen_firmware_array.py,
 * and the Intel 7260 ucode carried alongside them) */

#if WLAN_NIC_USB
extern const uint8_t rtl8188eufw_data[];
extern const size_t rtl8188eufw_size;
#endif

#if WLAN_NIC_PCIE
extern int pcie_glue_init(void);
#endif

/* adapter registry state (the port-core section at the bottom of the file
 * owns the logic; wlan_start reads the preference) */
static const struct wlan_port_adapter *wlan_registry[WLAN_PORT_NIC_MAX];
static unsigned wlan_registered;
static const struct wlan_port_adapter *wlan_active;
static char wlan_nic_pref_name[16];
static const char *wlan_nic_pref;

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
#if WLAN_NIC_USB
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
#endif
#if WLAN_NIC_PCIE
	{
		extern const uint8_t iwlwifi7260_17_ucode_data[];
		extern const size_t iwlwifi7260_17_ucode_size;

		if (wlan_port_firmware_register("iwlwifi-7260-17.ucode",
			iwlwifi7260_17_ucode_data,
			(size_t) iwlwifi7260_17_ucode_size) != 0) {
			printf("wlan: iwm firmware registration failed\n");
			return -1;
		}
	}
#endif

	/* platform power-up + ehci_init + config_found: the enumeration,
	 * hub exploration and urtwn attach (with its firmware load) run
	 * on the calling thread and the threads the chain spawns.
	 *
	 * Each line is brought up only if the image carries it (WLAN_NIC at build
	 * time); in a both-lines image the `wlan nic` preference can skip one of
	 * them, which is a convenience, not the mechanism - a debug image is
	 * built for exactly one line, so neither the 4 MB heap (iwm's RX ring
	 * alone is ~1.1 MB) nor the bring-up order couples the two. */
#if WLAN_NIC_USB
	if (wlan_nic_pref != NULL && strcmp(wlan_nic_pref, "iwm") == 0) {
		printf("wlan: usb line skipped (preference: %s)\n",
		    wlan_nic_pref);
	} else if (usb_platform_init() != 0) {
		printf("wlan: usb platform init failed\n");
		return -1;
	}
#endif
	/* The pcie world runs after the usb line: usb keeps its proven boot
	 * order.  A pcie failure must not flip the started flag back - the usb
	 * world is already up and a re-run would re-init the EHCI/xHCI hosts. */
#if WLAN_NIC_PCIE
	if (wlan_nic_pref != NULL && strcmp(wlan_nic_pref, "urtwn") == 0) {
		printf("wlan: pcie line skipped (preference: %s)\n",
		    wlan_nic_pref);
	} else if (pcie_glue_init() != 0) {
		printf("wlan: pcie glue init failed (usb line stays up;"
		    " reboot to retry pcie)\n");
	}
#endif
	wlan_started = 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* port core: the bus-neutral shell the shell commands dispatch onto
 * (see port.h).  The registry keeps every adapter that attached and one
 * of them is active: the preference (set from the shell with `wlan nic`)
 * picks which, and without a preference the first to attach wins. */

void wlan_port_nic_pref_set(const char *name) {
	/* The name arrives from the shell's argv, whose storage is reused by
	 * the next command - keep our own copy. */
	if (name == NULL || strcmp(name, "auto") == 0) {
		wlan_nic_pref = NULL;
		return;
	}
	strncpy(wlan_nic_pref_name, name, sizeof(wlan_nic_pref_name) - 1);
	wlan_nic_pref_name[sizeof(wlan_nic_pref_name) - 1] = '\0';
	wlan_nic_pref = wlan_nic_pref_name;
}

const char *wlan_port_nic_pref_get(void) {
	return wlan_nic_pref;
}

static const struct wlan_port_adapter *wlan_registry_find(const char *name) {
	unsigned i;

	for (i = 0; i < wlan_registered; i++) {
		if (strcmp(wlan_registry[i]->name, name) == 0) {
			return wlan_registry[i];
		}
	}
	return NULL;
}

void wlan_port_adapter_register(const struct wlan_port_adapter *adapter) {
	if (wlan_registry_find(adapter->name) != NULL || adapter->name == NULL) {
		return;
	}
	if (wlan_registered >= WLAN_PORT_NIC_MAX) {
		printf("wlan: adapter registry full, '%s' ignored\n",
		    adapter->name);
		return;
	}
	wlan_registry[wlan_registered++] = adapter;

	if (wlan_active == NULL) {
		if (wlan_nic_pref == NULL ||
		    strcmp(wlan_nic_pref, adapter->name) == 0) {
			wlan_active = adapter;
		} else {
			printf("wlan: adapter '%s' registered (inactive;"
			    " preference is '%s')\n", adapter->name,
			    wlan_nic_pref);
			return;
		}
	}
	printf("wlan: adapter '%s' registered\n", adapter->name);
}

/* The effective active adapter.  A preference that nothing matched (typo, or
 * a NIC whose attach failed) must not leave the port dead: fall back to the
 * first one that attached.  Deferring the fallback to first use is what keeps
 * a preference like "urtwn" working - iwm attaches first and stays inactive,
 * and the USB attach that arrives later takes the slot. */
static const struct wlan_port_adapter *wlan_active_eff(void) {
	if (wlan_active == NULL && wlan_registered > 0) {
		return wlan_registry[0];
	}
	return wlan_active;
}

int wlan_port_select(const char *name) {
	const struct wlan_port_adapter *a;

	if (name == NULL) {
		return -1;
	}
	a = wlan_registry_find(name);
	if (a == NULL) {
		return -1;
	}
	if (a != wlan_active_eff()) {
		wlan_active = a;
		printf("wlan: adapter '%s' selected\n", a->name);
	}
	return 0;
}

const char *wlan_port_active_name(void) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	return a != NULL ? a->name : NULL;
}

void *wlan_port_get_ic(void) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	return a != NULL ? a->ic : NULL;
}

int wlan_port_up(void) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	if (a == NULL || a->up == NULL) {
		return -1;
	}
	return a->up();
}

int wlan_port_scan(const uint8_t *ssid, size_t len) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	if (a == NULL || a->scan == NULL) {
		return -1;
	}
	return a->scan(ssid, len);
}

int wlan_port_xmit(const uint8_t *frame, size_t len) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	if (a == NULL || a->xmit == NULL) {
		return -1;
	}
	return a->xmit(frame, len);
}

int wlan_port_get_hwaddr(uint8_t addr[6]) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	if (a == NULL || a->get_hwaddr == NULL) {
		return -1;
	}
	return a->get_hwaddr(addr);
}

void wlan_port_status_dump(void) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	if (a == NULL || a->status_dump == NULL) {
		printf("wlan: no adapter\n");
		return;
	}
	a->status_dump();
}

void wlan_port_scan_dump(void) {
	const struct wlan_port_adapter *a = wlan_active_eff();

	if (a == NULL || a->scan_dump == NULL) {
		printf("wlan: no adapter\n");
		return;
	}
	a->scan_dump();
}

void wlan_port_deinit(void) {
}
