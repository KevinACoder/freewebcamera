/*
 * @file
 * @brief Internal glue of the CMSIS-RTOS2 port: osal entry points,
 * the serializer helpers and the firmware registry.
 */

#ifndef WLAN_PORT_CMSIS_H_
#define WLAN_PORT_CMSIS_H_

#include <stdint.h>
#include <stddef.h>

#include <port/port.h>
#include <port/osal/wlan_port_core.h>

/* One-time osal setup: creates the serializer and the tsleep
 * primitives. Call before wlan_port_init(). */
void wlan_osal_cmsis_init(void);

/* Port serializer internals (lock/unlock are in port/port.h): sleep
 * paths drop the lock with suspend/resume, owner is for assertions. */
void *wlan_port_serializer_owner(void);
int wlan_port_serializer_suspend(void);
void wlan_port_serializer_resume(int depth);

/* Firmware registry: the integrator registers the blobs it carries
 * (embedded arrays, a file system, ...) under their driver image
 * names before the first attach. Entries are never freed. */
int wlan_port_firmware_register(const char *name, const uint8_t *data,
	size_t size);

/* Memory hooks behind the compat malloc(9)/kmem(9) shells. CMSIS-RTOS2
 * has no allocation API; the weak defaults use malloc/free, an
 * integrator with a dedicated heap overrides them. */
void *wlan_osal_alloc(size_t size);
void wlan_osal_free(void *p);

#endif /* WLAN_PORT_CMSIS_H_ */
