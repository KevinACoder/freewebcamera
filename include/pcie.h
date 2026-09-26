/*
 * @file   pcie.h
 * @brief  PCIe host controller interface - a CMSIS-Driver gap.
 *
 * CMSIS-Driver ships 18 Driver_*.h headers and none of them covers PCIe,
 * NVMe, SATA/AHCI or Video/UVC. This header fills the PCIe gap but keeps the
 * CMSIS idiom exactly: an ops struct of function pointers plus
 * ARM_DRIVER_VERSION, so that if ARM ever defines Driver_PCIe.h this can be
 * switched over rather than rewritten.
 *
 * Scope at M0: the interface is FROZEN but has no implementation. The RK3568
 * DesignWare PCIe driver arrives with milestone M3 (NVMe) / M5 (iwm), and is
 * to be rewritten from the register sequences verified in the lab's embox
 * driver.
 *
 * CONTRACT:
 *  - All calls are task-context only; none is ISR-safe.
 *  - `bus` is the controller instance (0 or 1); instances are independent.
 *  - Transfer/bus-master operations complete asynchronously and signal via
 *    the event callback, which runs in ISR context.
 *  - Who allocates: the caller owns every buffer passed in; the driver never
 *    frees caller memory.
 */

#ifndef FREEWEBCAMERA_PCIE_H
#define FREEWEBCAMERA_PCIE_H

#include <stdbool.h>
#include <stdint.h>

#include "Driver_Common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCIE_API_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(0, 1)

/* PCIe events */
#define PCIE_EVENT_LINK_UP        (1UL << 0)
#define PCIE_EVENT_LINK_DOWN      (1UL << 1)
#define PCIE_EVENT_TRANSFER_DONE  (1UL << 2)
#define PCIE_EVENT_ERROR          (1UL << 3)

typedef void (*PCIE_SignalEvent_t)(uint32_t event, uint32_t bus);

typedef struct _PCIE_CAPABILITIES {
    uint8_t  max_lanes;         /* lanes trained or available */
    uint8_t  link_speed;        /* 1 = Gen1, 2 = Gen2, 3 = Gen3 */
    uint8_t  msi_capable;
    uint8_t  msix_capable;
    uint32_t config_space_size;
    uint32_t mmio_window_count;
} PCIE_CAPABILITIES;

typedef struct _PCIE_CONFIG_ACCESS {
    uint32_t bus;
    uint32_t device;
    uint32_t function;
    uint32_t offset;
    uint32_t *value;
} PCIE_CONFIG_ACCESS;

typedef struct _ARM_DRIVER_PCIE {
    ARM_DRIVER_VERSION   (*GetVersion)      (void);
    PCIE_CAPABILITIES    (*GetCapabilities)(uint32_t bus);
    int32_t              (*Initialize)      (uint32_t bus, PCIE_SignalEvent_t cb_event);
    int32_t              (*Uninitialize)    (uint32_t bus);
    int32_t              (*PowerControl)    (uint32_t bus, uint32_t state);

    /* Link management. `state` receives the LTSSM state readback, which is
     * the only reliable way to tell "trained" from "still polling". */
    int32_t              (*LinkUp)          (uint32_t bus);
    int32_t              (*GetLinkState)    (uint32_t bus, uint32_t *state);

    /* Configuration space access, 32-bit aligned. */
    int32_t              (*ConfigRead)      (const PCIE_CONFIG_ACCESS *access);
    int32_t              (*ConfigWrite)     (const PCIE_CONFIG_ACCESS *access);

    /* Bus enumeration. Enumerates downstream of `bus` and reports how many
     * functions were found. */
    int32_t              (*Enumerate)       (uint32_t bus, uint32_t *function_count);

    /* Address translation for a BAR window. */
    int32_t              (*MapWindow)       (uint32_t bus, uint32_t window,
                                             uint32_t *cpu_addr, uint32_t *size);
} ARM_DRIVER_PCIE;

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_PCIE_H */
