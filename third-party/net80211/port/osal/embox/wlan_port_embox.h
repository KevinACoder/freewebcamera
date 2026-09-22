/*
 * @file
 * @brief Internal glue of the embox port: the serializer helpers.
 *
 * The OS-agnostic port core state (interface table) lives in
 * port/osal/wlan_port_core.h, shared with the other ports.
 */

#ifndef WLAN_PORT_EMBOX_H_
#define WLAN_PORT_EMBOX_H_

#include <port/osal/wlan_port_core.h>

/* Port serializer: replaces the splnet() discipline of the imported
 * PCI drivers (spl is a no-op on this port). Driver adapter entries
 * take it; tsleep drops it around the wait. lock/unlock are declared
 * in port/port.h. */
void *wlan_port_serializer_owner(void);

/* Release the lock around a sleep and restore the saved hold count
 * (0 when the caller was not holding it). */
int wlan_port_serializer_suspend(void);
void wlan_port_serializer_resume(int depth);

#endif /* WLAN_PORT_EMBOX_H_ */
