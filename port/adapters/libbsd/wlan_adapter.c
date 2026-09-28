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

/* The build configuration (generated).  The bus-level bring-up below keys on
 * CONFIG_BUS_* and the per-driver work on CONFIG_NIC_*: the camera rides the
 * USB bus without needing any USB wireless driver. */
#include "config.h"

#include "board.h"

/* the net80211 prologue the driver adapters carry: wlan_port_adapter_
 * for_ifnet/opmode read ic_ifp/ic_opmode out of the adapter's ic */
#include <sys/queue.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net80211/ieee80211_netbsd.h>
#include <net80211/ieee80211_var.h>

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
		.stack_size = CONFIG_WLAN_WORKER_STACK,
	};

	return (void *) osThreadNew((osThreadFunc_t) run, arg, &attr);
}

/* ------------------------------------------------------------------ */
/* firmware: the blobs embedded at build time (generated from the
 * net80211 submodule's realtek dist by tools/gen_firmware_array.py,
 * and the Intel 7260 ucode carried alongside them). Each line's
 * bring-up compiles only when the image carries it (CONFIG_NIC_* at
 * build time - see configs/). */

#if CONFIG_NIC_URTWN
extern const uint8_t rtl8188eufw_data[];
extern const size_t rtl8188eufw_size;
#endif

#if CONFIG_NIC_IWM
extern int pcie_glue_init(void);
#endif

#if CONFIG_NIC_RTW8189F
#include "sdio.h"

extern const uint8_t rtw8189ffw_data[];
extern const size_t rtw8189ffw_size;
/* the rtw8189f chip driver TU (rtw8189f_reg.c) */
extern const struct wlan_chip_driver rtw8189f_driver;
/* SDIO has no autoconf hotplug hook (D51 of the frozen DESIGN): the port
 * claims the enumerated card through this explicit, idempotent probe. */
int wlan_sdio_probe(void);

/* the SDIO-side chip driver registry the bus probe scans (the USB and
 * PCIe lines attach through the NetBSD autoconf chain instead) */
const struct wlan_chip_driver *const wlan_chip_drivers[] = {
	&rtw8189f_driver,
	NULL,
};
#endif
/* adapter registry state (the port-core section at the bottom of the file
 * owns the logic; wlan_start reads the preference) */
static const struct wlan_port_adapter *wlan_registry[WLAN_PORT_NIC_MAX];
static unsigned wlan_registered;
static const struct wlan_port_adapter *wlan_active;
static char wlan_nic_pref_name[16];
static const char *wlan_nic_pref;

/* attachment notification (the lwIP bridge creates a per-adapter netif
 * here; NULL until the presentation layer installs itself) */
static wlan_port_attach_fn wlan_attach_fn;
static void *wlan_attach_arg;
static void wlan_attach_notify(const struct wlan_port_adapter *adapter);

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
	 * (ehci's level comes from EHCI_DEBUG_DEFAULT); the bus is up
	 * whenever anything rides it - a wireless line or the camera.
	 * usbdebug is usb.c's variable: it exists only when the bus and
	 * the history machinery are both compiled in. */
#if CONFIG_BUS_USB && !CONFIG_USB_BACKEND_CHERRYUSB
#if CONFIG_USB_DEBUG_DEFAULT
	{
		extern int usbdebug;

		usbdebug = CONFIG_USB_DEBUG_DEFAULT;
	}
#endif
#endif
#if CONFIG_NIC_URTWN
	if (wlan_port_firmware_register("rtl8188eufw.bin", rtl8188eufw_data,
		(size_t) rtl8188eufw_size) != 0) {
		printf("wlan: firmware registration failed\n");
		return -1;
	}
#endif
#if CONFIG_NIC_IWM
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
#if CONFIG_NIC_RTW8189F
	if (wlan_port_firmware_register("rtw8189f_fw.bin", rtw8189ffw_data,
	    (size_t) rtw8189ffw_size) != 0) {
		printf("wlan: rtw8189f firmware registration failed\n");
		return -1;
	}
#endif

	/* platform power-up + ehci_init + config_found: the enumeration,
	 * hub exploration and urtwn attach (with its firmware load) run
	 * on the calling thread and the threads the chain spawns.
	 *
	 * Each line is brought up only if the image carries it (CONFIG_NIC_* at
	 * build time); in a both-lines image the `wlan nic` preference can
	 * skip one of them, which is a convenience, not the mechanism - a
	 * debug image is built for exactly one line, so neither the 4 MB
	 * heap (iwm's RX ring alone is ~1.1 MB) nor the bring-up order
	 * couples the two. */
#if CONFIG_BUS_USB
	if (wlan_nic_pref != NULL && strcmp(wlan_nic_pref, "iwm") == 0) {
		printf("wlan: usb line skipped (preference: %s)\n",
		    wlan_nic_pref);
	} else {
#if CONFIG_USB_BACKEND_CHERRYUSB
		/* The CherryUSB backend has its own platform entry (domain +
		 * both EHCI roots + hub threads).  The attach chain runs there
		 * too: the hub thread enumerates and the wlan class hook calls
		 * into urtwn_reg.c. */
		extern int usbh_platform_start(void);

		if (usbh_platform_start() != 0) {
			printf("wlan: cherryusb platform init failed\n");
			return -1;
		}
#else
		if (usb_platform_init() != 0) {
			printf("wlan: usb platform init failed\n");
			return -1;
		}
#endif
	}
#endif
	/* The pcie world runs after the usb line: usb keeps its proven boot
	 * order.  A pcie failure must not flip the started flag back - the
	 * usb world is already up and a re-run would re-init the EHCI/xHCI
	 * hosts. */
#if CONFIG_NIC_IWM
	if (wlan_nic_pref != NULL && strcmp(wlan_nic_pref, "urtwn") == 0) {
		printf("wlan: pcie line skipped (preference: %s)\n",
		    wlan_nic_pref);
	} else if (pcie_glue_init() != 0) {
		printf("wlan: pcie glue init failed (usb line stays up;"
		    " reboot to retry pcie)\n");
	}
#endif
#if CONFIG_NIC_RTW8189F
	/* The SDIO line: enumerate the slot, then run the explicit claim
	 * probe. The started flag goes up first: the probe's readiness
	 * gate reads it. */
	wlan_started = 1;
	if (sdio_start() != 0) {
		printf("wlan: sdio slot enumeration failed\n");
	} else if (wlan_sdio_probe() != 0) {
		printf("wlan: no SDIO wlan card matched\n");
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
			wlan_attach_notify(adapter);
			return;
		}
	}
	printf("wlan: adapter '%s' registered\n", adapter->name);
	wlan_attach_notify(adapter);
}

static void wlan_attach_notify(const struct wlan_port_adapter *adapter) {
	if (wlan_attach_fn != NULL) {
		wlan_attach_fn(adapter, wlan_attach_arg);
	}
}

void wlan_port_set_attach_notify(wlan_port_attach_fn fn, void *arg) {
	unsigned i;

	wlan_attach_fn = fn;
	wlan_attach_arg = arg;
	/* replay: the bridge installs itself from boot (net_start), the
	 * adapters arrive later with wlan start - but a re-install after
	 * wlan start must still see everything already registered */
	for (i = 0; i < wlan_registered; i++) {
		wlan_attach_fn(wlan_registry[i], wlan_attach_arg);
	}
}

const struct wlan_port_adapter *wlan_port_adapter_for_ifnet(void *ifp) {
	unsigned i;

	if (ifp == NULL) {
		return NULL;
	}
	for (i = 0; i < wlan_registered; i++) {
		const struct ieee80211com *ic = wlan_registry[i]->ic;

		if (ic != NULL && ic->ic_ifp == ifp) {
			return wlan_registry[i];
		}
	}
	return NULL;
}

int wlan_port_adapter_is_hostap(const struct wlan_port_adapter *adapter) {
	const struct ieee80211com *ic = adapter != NULL ? adapter->ic : NULL;

	return ic != NULL && ic->ic_opmode == IEEE80211_M_HOSTAP;
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

/* Registry lookup without touching the active selection: the HOSTAP
 * entry point (wlan_ap.c) drives a named adapter while the shell focus
 * stays wherever the user left it. */
const struct wlan_port_adapter *wlan_port_adapter_find(const char *name) {
	return name != NULL ? wlan_registry_find(name) : NULL;
}

/* Bring a named adapter up (NULL = the active one).  The two-NIC images
 * need this: the AP leg must not ride on whichever adapter happens to
 * be active. */
int wlan_port_up_for(const char *name) {
	const struct wlan_port_adapter *a = name != NULL ?
	    wlan_registry_find(name) : wlan_active_eff();

	if (a == NULL || a->up == NULL) {
		return -1;
	}
	return a->up();
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
