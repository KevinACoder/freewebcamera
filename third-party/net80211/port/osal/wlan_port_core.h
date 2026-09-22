/*
 * @file
 * @brief OS-agnostic port core state shared by every port.
 *
 * The port presents one interface per attached chip. The interface
 * table and the per-interface shell are OS-independent, so they live
 * here: the chip driver registry TUs and the bus backends reference
 * them without including a specific osal's internal glue header.
 */

#ifndef NET80211_WLAN_PORT_CORE_H_
#define NET80211_WLAN_PORT_CORE_H_

struct ifnet;
#include <port/port.h>
#include <port/bus/usb/port_usb.h>

#define WLAN_PORT_MAX_IF 2
#define WLAN_PORT_MAX_BULK_EP 4

struct wlan_port_iface {
	struct wlan_usb_dev usb;
	const struct wlan_chip_driver *drv;
	struct ifnet *if_shell; /* BSD ifnet shell, owned by the shim */
	void *shim_priv; /* usbd_device / softc world */
	int attached;
};

extern struct wlan_port_iface *wlan_port_ifs[WLAN_PORT_MAX_IF];
extern int wlan_port_if_n;

#endif /* NET80211_WLAN_PORT_CORE_H_ */
