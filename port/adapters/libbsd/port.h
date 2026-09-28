/*
 * @file
 * @brief The stable port interface of the net_80211 library.
 *
 * A port adapts the imported NetBSD net80211 stack and chip drivers to
 * one operating environment. The library never includes an OS header
 * directly: the NetBSD kernel API surface the imported code expects is
 * declared by the shadow headers in compat/netbsd/ and implemented by
 * the port, while the pieces that differ structurally between OSes go
 * through this header.
 *
 * Ports live in three categories under port/:
 *   osal/<os>/    the OS adaptation (locks, threads, timers, firmware
 *                 storage) behind the compat/netbsd/ declarations,
 *   bus/<bus>/    one directory per bus backend (usb/ with its
 *                 port_usb.h types, pcie/ and sd/ reserved), bringing
 *                 an attached device to the chip driver,
 *   net/<stack>/  the presentation layer (how the wlan interface
 *                 appears to the host stack: embox netdev + cfg80211,
 *                 lwip netif, ...).
 *
 * This header is the bus-agnostic contract: firmware lookup,
 * presentation hooks, the chip driver registry and the port lifecycle.
 * Bus-specific types sit next to their backend (port/bus/usb/port_usb.h
 * for USB) and are only visible as opaque structs here.
 *
 * The set of compiled-in chip drivers is discovered through
 * WLAN_CHIP_DRIVERS (a weak NULL-terminated array), so adding a driver
 * never touches a port.
 */

#ifndef NET80211_PORT_H_
#define NET80211_PORT_H_

#include <stdint.h>
#include <stddef.h>

/* Bus types; backends include the full definitions from
 * port/bus/<bus>/port_<bus>.h. */
struct wlan_usb_dev;
struct wlan_usb_id;
struct wlan_pcie_dev;
struct wlan_pcie_id;
struct wlan_sdio_dev;
struct wlan_sdio_id;

/* ------------------------------------------------------------------
 * Firmware
 *
 * Ports resolve a driver firmware name to bytes; embedding or loading
 * from a file system is up to the port. The blob is not freed.
 */

struct wlan_firmware {
	const uint8_t *data;
	size_t size;
};

int wlan_port_firmware_get(const char *name, struct wlan_firmware *fw);

/* ------------------------------------------------------------------
 * Net attachment (presentation hooks)
 *
 * net80211 runs on a BSD ifnet shell that the port owns. The port
 * decides how that shell maps onto its own stack.
 */

struct wlan_port_ifops {
	/* The interface went up/down (net80211 INIT <-> RUN transitions). */
	int (*up)(void *if_priv);
	int (*down)(void *if_priv);
};

/* ------------------------------------------------------------------
 * Frame delivery (presentation hooks)
 *
 * Data frames that pass the net80211 input path leave the library
 * through these hooks.  The presentation layer registers handlers for
 * the frames it wants; without a handler frames are dropped as before.
 * Both run in the USB worker context: the handlers must copy the frame
 * and return without blocking.
 *
 * Every hook receives the adapter the frame came from: a two-NIC image
 * runs one net80211 instance per adapter and the sinks route per
 * adapter (the lwIP bridge owns a netif per adapter).
 */

struct wlan_port_adapter;

/* EAPOL (ethertype 0x888e) frames; buf points at the payload behind
 * the 14-byte ethernet header, src is the ethernet source address. */
typedef void (*wlan_eapol_rx_fn)(const struct wlan_port_adapter *adapter,
    const uint8_t src[6], const uint8_t *buf, size_t len, void *arg);

/* Every other delivered frame, as a full ethernet frame. */
typedef void (*wlan_data_rx_fn)(const struct wlan_port_adapter *adapter,
    const uint8_t *frame, size_t len, void *arg);

void wlan_port_set_eapol_rx(wlan_eapol_rx_fn fn, void *arg);
void wlan_port_set_data_rx(wlan_data_rx_fn fn, void *arg);

enum wlan_port_event {
	WLAN_PORT_SCAN_DONE,
	WLAN_PORT_ASSOC,
	WLAN_PORT_DISASSOC,
};

/* Borrowed address, valid only during the callback. Consumers copy and
 * queue notifications; they must not re-enter the protocol state machine. */
typedef void (*wlan_event_fn)(const struct wlan_port_adapter *adapter,
    enum wlan_port_event event, const uint8_t *addr, void *arg);
void wlan_port_set_event_handler(wlan_event_fn fn, void *arg);

/* Resolve the adapter that owns an ifnet shell (NULL when the ifnet
 * does not belong to any registered adapter).  The RX dispatch in the
 * ifnet shell uses this to stamp frames with their origin. */
const struct wlan_port_adapter *wlan_port_adapter_for_ifnet(void *ifp);

/* The adapter's net80211 role; the lwIP bridge keys its link
 * semantics on it (an AP netif stays up across station joins/leaves, a
 * station netif follows association). */
int wlan_port_adapter_is_hostap(const struct wlan_port_adapter *adapter);

/* Attachment notification: called once per adapter that registers, in
 * registration order, plus a replay of everything already registered
 * when the presentation layer installs itself after wlan_start(). */
typedef void (*wlan_port_attach_fn)(const struct wlan_port_adapter *adapter,
    void *arg);
void wlan_port_set_attach_notify(wlan_port_attach_fn fn, void *arg);

/* Start one complete scan on the device worker, with no automatic join. */
int wlan_port_scan(const uint8_t *ssid, size_t len);

/* Send a full ethernet frame out of the wlan interface (queued to the
 * ifnet, encrypted/encapsulated by net80211). Returns len or -1. */
int wlan_port_xmit(const uint8_t *frame, size_t len);

/* The interface hardware address (after attach). */
int wlan_port_get_hwaddr(uint8_t addr[6]);

/* Bring the first attached interface up (firmware load + power on).
 * Provided by the port core on top of the driver up hooks. */
int wlan_port_up(void);

/* Diagnostics: the attached driver's softc/node dump and the net80211
 * scan table. Provided by the port core. */
void wlan_port_status_dump(void);
void wlan_port_scan_dump(void);

/* Focus the shell hooks on a named driver adapter. The adapter must have
 * attached; the port keeps a small registry of everything that did. */
int wlan_port_select(const char *name);
const char *wlan_port_active_name(void);

/* Registry lookup without changing the active selection, and the
 * per-adapter up (NULL = active).  The HOSTAP entry point drives a named
 * adapter this way in two-NIC images. */
struct wlan_port_adapter;
const struct wlan_port_adapter *wlan_port_adapter_find(const char *name);
int wlan_port_up_for(const char *name);

/* Adapter-scoped operations for consumers that pin one adapter at bind
 * time (the supplicant bridge): the shell focus may move to the other
 * NIC of a two-NIC image while these keep addressing the bound one.
 * for_ic() resolves the adapter owning a given ieee80211com. */
const struct wlan_port_adapter *wlan_port_adapter_active(void);
const struct wlan_port_adapter *wlan_port_adapter_for_ic(const void *ic);
int wlan_port_adapter_scan(const struct wlan_port_adapter *adapter,
    const uint8_t *ssid, size_t len);
int wlan_port_adapter_xmit(const struct wlan_port_adapter *adapter,
    const uint8_t *frame, size_t len);
int wlan_port_adapter_hwaddr(const struct wlan_port_adapter *adapter,
    uint8_t addr[6]);

/* How many NICs may attach at once (the registry bound). */
#define WLAN_PORT_NIC_MAX 4

/* Which adapter the port prefers: a driver name, or NULL for "whichever
 * attaches first".  Set before wlan_start() to keep a NIC out of the active
 * slot - the PCIe attach is synchronous and would otherwise always win
 * against the deferred USB one - and for "urtwn" the PCIe bring-up is
 * skipped entirely, so the two NICs are testable in one boot recipe. */
void wlan_port_nic_pref_set(const char *name);
const char *wlan_port_nic_pref_get(void);

/* The active adapter's ieee80211com (NULL before attach). */
void *wlan_port_get_ic(void);

/* ------------------------------------------------------------------
 * Driver registry
 */

enum wlan_bus_type {
	WLAN_BUS_USB = 1,
	WLAN_BUS_PCIE = 2,
	WLAN_BUS_SDIO = 3,
};

/* Control & diagnostics hooks a driver adapter offers the port shell.
 * Every entry may be NULL; the port core dispatches onto the first
 * adapter that registered. */
struct wlan_port_adapter {
	const char *name;
	int (*up)(void);
	int (*scan)(const uint8_t *ssid, size_t len);
	int (*xmit)(const uint8_t *frame, size_t len);
	int (*get_hwaddr)(uint8_t addr[6]);
	void (*status_dump)(void);
	void (*scan_dump)(void);
	/* the adapter's ieee80211com, for the supplicant bridge; set at
	 * attach time (the softc does not exist when the table is
	 * declared) */
	void *ic;
};

/* Called once by the driver adapter before/at attach time. */
void wlan_port_adapter_register(const struct wlan_port_adapter *adapter);

struct wlan_chip_driver {
	const char *name;
	enum wlan_bus_type bus;
	/* USB match table (WLAN_BUS_USB), terminated by vid==0 && pid==0. */
	const struct wlan_usb_id *usb_ids;
	/* PCI match table (WLAN_BUS_PCIE), terminated by vendor==0. */
	const struct wlan_pcie_id *pcie_ids;
	/* SDIO match table (WLAN_BUS_SDIO), CIS manufacturer/product of
	 * function 0, terminated by vendor==0. */
	const struct wlan_sdio_id *sdio_ids;

	/* Attach the device: bring the chip up, load the firmware,
	 * ieee80211_ifattach. bus_dev is the port device of the driver's
	 * bus (struct wlan_usb_dev / struct wlan_pcie_dev); if_priv is the
	 * port-owned ifnet shell. Returns 0 on success; on failure the
	 * port closes the device. */
	int (*attach)(void *bus_dev, void *if_priv);
	void (*detach)(void *if_priv);
	void (*stop)(void *if_priv);
};

/* Each driver translation unit exports one global
 * `const struct wlan_chip_driver <name>_driver`. The port defines its
 * own NULL-terminated `wlan_chip_drivers[]` listing the drivers it was
 * built with - explicit, so ports and drivers stay decoupled without
 * relying on linker sections or constructors. */

/* ------------------------------------------------------------------
 * Port lifecycle
 *
 * Called once by the environment before any use. The port scans the
 * registry, claims matching devices and presents one net interface
 * per attached chip (naming per presentation layer).
 */

/* wlan_port_init()/wlan_port_deinit() are provided per port; the
 * embox port wires wlan_port_init as its unit init. */
void wlan_port_deinit(void);

/* Explicit SDIO claim probe (the SDIO counterpart of the USB autoconf
 * chain): matches the enumerated SDIO card's function-0 CIS against the
 * WLAN_BUS_SDIO entries of wlan_chip_drivers[] and attaches the winner.
 * SDIO has no hotplug hook, so the call is explicit and idempotent -
 * run it after wlan_start() and again after a re-enumeration. Provided
 * by the SDIO bus backend (wlan_sdio_claim.c). */
int wlan_sdio_probe(void);

/* ------------------------------------------------------------------
 * Driver serialization
 *
 * The imported drivers rely on the splnet() discipline of their host
 * kernel to serialize the interrupt, state-machine and transmit
 * contexts. The port provides a reentrant lock instead; it is taken
 * around every driver entry and around the interface transmit path.
 */
void wlan_port_serializer_lock(void);
void wlan_port_serializer_unlock(void);

/* Diagnostic dump of the serializer's live state (owner/depth) and the
 * recent lock/unlock operation ring; also the panic path's forensics. */
void wlan_ser_dump(void);

/* Monotonic milliseconds, for the usbdi shim's xfer-timeout watchdog
 * (the stand-in for the NetBSD callout that arms ux_timeout). Wrap
 * around is tolerated: deadlines are compared as signed deltas. */
unsigned int wlan_port_now_ms(void);

#endif /* NET80211_PORT_H_ */
